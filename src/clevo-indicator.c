/*
 ============================================================================
 Name        : clevo-indicator.c
 Author      : AqD <iiiaqd@gmail.com>
 Version     : 0.1
 Description : Ubuntu fan control indicator for Clevo laptops

 Based on http://www.association-apml.fr/upload/fanctrl.c by Jonas Diemer
 (diemer@gmx.de)

 ============================================================================

 TEST:
 gcc clevo-indicator.c -o clevo-indicator `pkg-config --cflags --libs appindicator3-0.1` -lm
 sudo chown root clevo-indicator
 sudo chmod u+s clevo-indicator

 Run as effective uid = root, but uid = desktop user (in order to use indicator).

 ============================================================================
 Auto fan control algorithm:

 The algorithm is to replace the builtin auto fan-control algorithm in Clevo
 laptops which is apparently broken in recent models such as W350SSQ, where the
 fan doesn't get kicked until both of GPU and CPU are really hot (and GPU
 cannot be hot anymore thanks to nVIDIA's Maxwell chips). It's far more
 aggressive than the builtin algorithm in order to keep the temperatures below
 60°C all the time, for maximized performance with Intel turbo boost enabled.

 Version 0.1: added experimental support for two fans based on Clevo NH55RDQ
 by https://github.com/deebfeast

 ============================================================================
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/io.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <chrono>

#include <libayatana-appindicator3-0.1/libayatana-appindicator/app-indicator.h>

#include <nvml.h>

#define NAME "clevo-indicator"




#define EC_SC 0x66
#define EC_SC2 0x67
#define EC_DATA 0x62
#define EC_DATA2 0x63

#define IBF 1
#define IBF2 2
#define OBF 0
#define EC_SC_READ_CMD 0x80

/* EC registers can be read by EC_SC_READ_CMD or /sys/kernel/debug/ec/ec0/io:
 *
 * 1. modprobe ec_sys
 * 2. od -Ax -t x1 /sys/kernel/debug/ec/ec0/io
 */

#define EC_REG_SIZE 0x100
#define EC_REG_CPU_TEMP 0x07
#define EC_REG_GPU_TEMP 0xCD
//0x69
#define EC_REG_FAN_DUTY 0xCE
#define EC_REG_FA2_DUTY 0xCF
#define EC_REG_FAN_RPMS_HI 0xD0
#define EC_REG_FAN_RPMS_LO 0xD1
#define EC_REG_FA2_RPMS_HI 0xD2
#define EC_REG_FA2_RPMS_LO 0xD3

#define MAX_FAN_RPM 4400.0

typedef enum {
    NA = 0, AUTO = 1, MANUAL = 2
} MenuItemType;

static void main_init_share(void);
static int main_ec_worker(void);
static void main_ui_worker(int argc, char** argv);
static void main_on_sigchld(int signum);
static void main_on_sigterm(int signum);
static int main_dump_fan(void);
static int main_test_fan(int duty_percentage);
static gboolean ui_update(gpointer user_data);
static void ui_command_set_fan(long fan_duty);
//static void ui_command_set_mode(long fd);
static void ui_command_quit(gchar* command);
static void ui_toggle_menuitems(int fan_duty);
static void ec_on_sigterm(int signum);
static int ec_init(void);
static int ec_auto_duty_adjust(void);
static int ec_query_cpu_temp(void);
static int ec_query_gpu_temp(void);
static int ec_query_fan_duty(int fan_number);
static int ec_query_fan_rpms(void);
static int ec_query_fa2_rpms(void);
static int ec_set_use_vfan_chart(void);
static int ec_write_fan_duty(int duty_percentage, int fan_number);
static int ec_io_wait(const uint32_t port, const uint32_t flag, const char value);
static uint8_t ec_io_read(const uint32_t port);
static int ec_io_do(const uint32_t cmd, const uint32_t port, const uint8_t value);
static int calculate_fan_duty(int raw_duty);
static int calculate_fan_rpms(int raw_rpm_high, int raw_rpm_low);
static int check_proc_instances(const char* proc_name);
static void get_time_string(char* buffer, size_t max, const char* format);
static void signal_term(__sighandler_t handler);
static int ec_set_use_vfan_chart(void);
static int dump_ec(void);

static AppIndicator* indicator = NULL;

struct {
    char label[256];
    GCallback callback;
    long option;
    MenuItemType type;
    GtkWidget* widget;

}static menuitems[] = {
        { "Set Performance Auto", G_CALLBACK(ui_command_set_fan), -2, MANUAL, NULL },
        { "Set Quiet Auto", G_CALLBACK(ui_command_set_fan), -1, AUTO, NULL },
        { "", NULL, 0, NA, NULL },                                                 /// startIndex starts                      
        { "Set FAN to  20%", G_CALLBACK(ui_command_set_fan), 20, MANUAL, NULL },
        { "Set FAN to  30%", G_CALLBACK(ui_command_set_fan), 30, MANUAL, NULL },
        { "Set FAN to  40%", G_CALLBACK(ui_command_set_fan), 40, MANUAL, NULL },
        { "Set FAN to  50%", G_CALLBACK(ui_command_set_fan), 50, MANUAL, NULL },
        { "Set FAN to  60%", G_CALLBACK(ui_command_set_fan), 60, MANUAL, NULL },
        { "Set FAN to  70%", G_CALLBACK(ui_command_set_fan), 70, MANUAL, NULL },
        { "Set FAN to  80%", G_CALLBACK(ui_command_set_fan), 80, MANUAL, NULL },
        { "Set FAN to  90%", G_CALLBACK(ui_command_set_fan), 90, MANUAL, NULL },
        { "Set FAN to 100%", G_CALLBACK(ui_command_set_fan), 100, MANUAL, NULL },
        { "", NULL, 0L, NA, NULL },
        { "Quit", G_CALLBACK(ui_command_quit), 0L, NA, NULL }
};
int startIndex = 1;

static int menuitem_count = (sizeof(menuitems) / sizeof(menuitems[0]));

typedef struct nvidia_device_t {
    nvmlDevice_t device;
    nvmlTemperatureSensors_t sensor; // NVML_TEMPERATURE_GPU
    char name[NVML_DEVICE_NAME_BUFFER_SIZE];
    unsigned int gpu_temp;
    unsigned int device_count;

} nvidia_device;

struct info{
    volatile int exit;
    volatile int cpu_temp;
    volatile int gpu_temp;
    volatile int fan_duty;
    volatile int fan_rpms;
    volatile int auto_duty;
    volatile int auto_duty_val;
    volatile int manual_next_fan_duty;
    volatile int manual_prev_fan_duty;
    volatile int performance_mode;
    volatile nvidia_device nvidia_devices[2];
}static *share_info = NULL;

nvidia_device* init_nvml(nvidia_device*, unsigned int*);
int nvml_query_gpu_temp(nvidia_device device);

static nvidia_device* init_nvml(volatile nvidia_device* nvidia_devices)
{
    nvmlReturn_t result;

    // First initialize NVML library
    result = nvmlInit();
    if (NVML_SUCCESS != result) {
        printf("Failed to initialize NVML: %s\n", nvmlErrorString(result));

        printf("Press ENTER to continue...\n");
        getchar();
        return NULL;
    }

    unsigned int num_devices;
    result = nvmlDeviceGetCount(&num_devices);
    
    if (NVML_SUCCESS != result) {
        printf("Failed to query device count: %s\n", nvmlErrorString(result));
        exit(EXIT_FAILURE);
    }

    //printf("Found %u devices\n\n", nvidia_devices->device_count);
    //printf("Listing devices:\n");

    nvmlPciInfo_t pci;

    for (unsigned int i = 0; i < num_devices; i++) {
        nvidia_devices[i].device_count = num_devices;
        nvidia_devices[i].sensor = NVML_TEMPERATURE_GPU;
        result = nvmlDeviceGetHandleByIndex(i, (nvmlDevice_t*)&nvidia_devices[i].device);
        result = nvmlDeviceGetName((nvmlDevice_t)nvidia_devices[i].device,
            (char*)nvidia_devices[i].name, NVML_DEVICE_NAME_BUFFER_SIZE);
        result = nvmlDeviceGetPciInfo((nvmlDevice_t)nvidia_devices[i].device, &pci);
        //printf("%u. %s [%s]\n", nvidia_devices[i].device, nvidia_devices[i].name, pci.busId);
        nvmlDeviceGetTemperature((nvmlDevice_t)nvidia_devices[i].device,
            (nvmlTemperatureSensors_t)nvidia_devices[i].sensor, (unsigned int*)&nvidia_devices[i].gpu_temp);
        //printf("TEMP NV: %d\n", nvidia_devices[i].gpu_temp);
    }
    return (nvidia_device*)nvidia_devices;
}

static unsigned int nvml_query_gpu_temp(volatile nvidia_device *device)
{
    nvmlDeviceGetTemperature(device->device, device->sensor, (unsigned int*)&device->gpu_temp);
    return (unsigned int)device->gpu_temp;
}

static pid_t parent_pid = 0;

int main(int argc, char* argv[]) 
{
    bool help_requested = false, set_vtable = false, set_dump_ec = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0) {
            help_requested = true;
            break;
        }
        else if(strcmp(argv[i], "-s") == 0) {
            set_vtable = true;
            break;
        }
        else if(strcmp(argv[i], "-d") == 0) {
            set_dump_ec = true;
            break;
        }
    }
    printf("Simple fan control utility for Clevo laptops\n");
    if (check_proc_instances(NAME) > 1) {
        printf("Multiple running instances!\n");
        char* display = getenv("DISPLAY");
        if (display != NULL && strlen(display) > 0) {
            int desktop_uid = getuid();
            setuid(desktop_uid);
            //
            gtk_init(&argc, &argv);
            GtkWidget* dialog = gtk_message_dialog_new(NULL, 
                    (GtkDialogFlags)0,
                    GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                    "Multiple running instances of %s!", NAME);
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
        }
        return EXIT_FAILURE;
    }
    if (ec_init() != EXIT_SUCCESS) {
        printf("unable to control EC: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    if (argc <= 1) {
        char* display = getenv("DISPLAY");
        if (display == NULL || strlen(display) == 0) {
            return main_dump_fan();
        } else {
            parent_pid = getpid();
            main_init_share();
            signal(SIGCHLD, &main_on_sigchld);
            signal_term(&main_on_sigterm);
            pid_t worker_pid = fork();
            if (worker_pid == 0) {
                signal(SIGCHLD, SIG_DFL);
                signal_term(&ec_on_sigterm);
                return main_ec_worker();
            } else if (worker_pid > 0) {
                main_ui_worker(argc, argv);
                share_info->exit = 1;
                waitpid(worker_pid, NULL, 0);
            } else {
                printf("unable to create worker: %s\n", strerror(errno));
                return EXIT_FAILURE;
            }
        }
    } else {
        if (help_requested) {
            printf(
                    "\n\
Usage: clevo-indicator [fan-duty-percentage]\n\
\n\
Dump/Control fan duty on Clevo laptops. Display indicator by default.\n\
\n\
Arguments:\n\
  [fan-duty-percentage]\t\tTarget fan duty in percentage, from 20 to 100\n\
  -?\t\t\t\tDisplay this help and exit\n\
\n\
Without arguments this program should attempt to display an indicator in\n\
the Ubuntu tray area for fan information display and control. The indicator\n\
requires this program to have setuid=root flag but run from the desktop user\n\
, because a root user is not allowed to display a desktop indicator while a\n\
non-root user is not allowed to control Clevo EC (Embedded Controller that's\n\
responsible of the fan). Fix permissions of this executable if it fails to\n\
run:\n\
    sudo chown root clevo-indicator\n\
    sudo chmod u+s  clevo-indicator\n\
\n\
Note any fan duty change should take 1-2 seconds to come into effect - you\n\
can verify by the fan speed displayed on indicator icon and also louder fan\n\
noise.\n\
\n\
In the indicator mode, this program would always attempt to load kernel\n\
module 'ec_sys', in order to query EC information from\n\
'/sys/kernel/debug/ec/ec0/io' instead of polling EC ports for readings,\n\
which may be more risky if interrupted or concurrently operated during the\n\
process.\n\
\n\
DO NOT MANIPULATE OR QUERY EC I/O PORTS WHILE THIS PROGRAM IS RUNNING.\n\
\n");
            return main_dump_fan();
        } 
        else if(set_vtable)
        {
            ec_set_use_vfan_chart();
        }
        else if(set_dump_ec)
        {
            dump_ec();
        }
        else 
        {
            int val = atoi(argv[1]);
            if (val < 0 || val > 100)
                    {
                printf("invalid fan duty %d!\n", val);
                return EXIT_FAILURE;
            }
            return main_test_fan(val);
        }
    }
    return EXIT_SUCCESS;
}

static void main_init_share(void) {
    void* shm = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED,
            -1, 0);
    share_info = (struct info*)shm;
    share_info->exit = 0;
    share_info->cpu_temp = 0;
    share_info->gpu_temp = 0;
    share_info->fan_duty = 0;
    share_info->fan_rpms = 0;
    share_info->auto_duty = 1;
    share_info->auto_duty_val = 0;
    share_info->manual_next_fan_duty = 0;
    share_info->manual_prev_fan_duty = 1;
    share_info->performance_mode = 0;
    
}

static int main_ec_worker(void) {
    init_nvml(share_info->nvidia_devices);
    setuid(0);
    system("modprobe ec_sys");
    while (share_info->exit == 0) {
        // check parent
        if (parent_pid != 0 && kill(parent_pid, 0) == -1) {
            printf("worker on parent death\n");
            break;
        }
        // write EC
        int new_fan_duty = share_info->manual_next_fan_duty;
        if (new_fan_duty != 0
                && new_fan_duty != share_info->manual_prev_fan_duty) {
            ec_write_fan_duty(new_fan_duty,1);
            ec_write_fan_duty(new_fan_duty,2);
            ec_write_fan_duty(new_fan_duty,3);
            share_info->manual_prev_fan_duty = new_fan_duty;
        }
        // read EC
        int io_fd = open("/sys/kernel/debug/ec/ec0/io", O_RDONLY, 0);
        if (io_fd < 0) {
            printf("unable to read EC from sysfs: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
        unsigned char buf[EC_REG_SIZE];
        ssize_t len = read(io_fd, buf, EC_REG_SIZE);
        switch (len) {
        case -1:
            printf("unable to read EC from sysfs: %s\n", strerror(errno));
            break;
        case 0x100:
            nvml_query_gpu_temp(&share_info->nvidia_devices[0]);
            nvml_query_gpu_temp(&share_info->nvidia_devices[1]);
            share_info->cpu_temp = buf[EC_REG_CPU_TEMP];
            share_info->gpu_temp = buf[EC_REG_GPU_TEMP];
            share_info->fan_duty = calculate_fan_duty(buf[EC_REG_FAN_DUTY]);
            share_info->fan_rpms = calculate_fan_rpms(buf[EC_REG_FAN_RPMS_HI],
                    buf[EC_REG_FAN_RPMS_LO]);
            /*
             printf("temp=%d, duty=%d, rpms=%d\n", share_info->cpu_temp,
             share_info->fan_duty, share_info->fan_rpms);
             */
            break;
        default:
            printf("wrong EC size from sysfs: %ld\n", len);
        }
        close(io_fd);
        // auto EC
        if (share_info->auto_duty == 1) {
            int next_duty = ec_auto_duty_adjust();
            if (next_duty != 0 && next_duty != share_info->auto_duty_val) {
                char s_time[256];
                get_time_string(s_time, 256, "%m/%d %H:%M:%S");
                // printf("%s CPU=%d°C, GPU=%d°C, auto fan duty to %d%%\n", s_time,
                printf("%s CPU=%d°C, auto fan duty to %d%%\n", s_time,
                        share_info->cpu_temp, next_duty);
                ec_write_fan_duty(next_duty,1);
                ec_write_fan_duty(next_duty,2);
                ec_write_fan_duty(next_duty,3);
                share_info->auto_duty_val = next_duty;
            }
        }
        //
        usleep(200 * 1000);
    }
    printf("worker quit\n");
    return EXIT_SUCCESS;
}

static void main_ui_worker(int argc, char** argv) {
    printf("Indicator...\n");
    int desktop_uid = getuid();
    setuid(desktop_uid);
    //
    gtk_init(&argc, &argv);
    //
    GtkWidget* indicator_menu = gtk_menu_new();
    for (int i = 0; i < menuitem_count; i++) {
        GtkWidget* item;
        if (strlen(menuitems[i].label) == 0) {
            item = gtk_separator_menu_item_new();
        } else {
            item = gtk_menu_item_new_with_label(menuitems[i].label);
            g_signal_connect_swapped(item, "activate",
                    G_CALLBACK(menuitems[i].callback), (void* ) menuitems[i].option);
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(indicator_menu), item);
        menuitems[i].widget = item;
    }
    gtk_widget_show_all(indicator_menu);
    //
    indicator = app_indicator_new(NAME, "brasero", APP_INDICATOR_CATEGORY_HARDWARE);
    g_assert(APP_IS_INDICATOR(indicator));
    app_indicator_set_label(indicator, "Init..", "XX");
    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ATTENTION);
    app_indicator_set_ordering_index(indicator, -2);
    app_indicator_set_title(indicator, "Clevo");
    app_indicator_set_menu(indicator, GTK_MENU(indicator_menu));
    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
    g_timeout_add(500, &ui_update, NULL);
    ui_toggle_menuitems(startIndex);
    gtk_main();
    printf("main on UI quit\n");
}

static void main_on_sigchld(int signum) {
    printf("main on worker quit signal\n");
    exit(EXIT_SUCCESS);
}

static void main_on_sigterm(int signum) {
    printf("main on signal: %s\n", strsignal(signum));
    if (share_info != NULL)
        share_info->exit = 1;
    exit(EXIT_SUCCESS);
}

static int main_dump_fan(void) {
    printf("Dump fan information\n");
    printf("  FAN Duty: %d%%\n", ec_query_fan_duty(1));
    printf("  FA2 Duty: %d%%\n", ec_query_fan_duty(2));
    printf("  FA3 Duty: %d%%\n", ec_query_fan_duty(2));
    printf("  FAN RPMs: %d RPM\n", ec_query_fan_rpms());
    printf("  FA2 RPMs: %d RPM\n", ec_query_fa2_rpms());
    printf("  CPU Temp: %d°C\n", ec_query_cpu_temp());
    printf("  GPU Temp: %d°C\n", ec_query_gpu_temp());
    main_init_share();
    init_nvml(share_info->nvidia_devices);
    for(int i = 0; i < share_info->nvidia_devices[0].device_count; i++) {
        printf("  GPU%s Temp: %d°C\n", std::to_string(i+1).c_str(), nvml_query_gpu_temp(&share_info->nvidia_devices[0]));
    }
    return EXIT_SUCCESS;
}

static int main_test_fan(int duty_percentage) {
    printf("Change fans duty to %d%%\n", duty_percentage);
    ec_write_fan_duty(duty_percentage,1);
    ec_write_fan_duty(duty_percentage,2);
    ec_write_fan_duty(duty_percentage,3);
    printf("\n");
    main_dump_fan();
    return EXIT_SUCCESS;
}

static gboolean ui_update(gpointer user_data) {
    char label[256];
    // sprintf(label, "%d℃ %d℃", share_info->cpu_temp, share_info->gpu_temp);
    sprintf(label, "%d℃", share_info->cpu_temp);
    app_indicator_set_label(indicator, label, "XXXXXX");
    char icon_name[256];
    double load = ((double) share_info->fan_rpms) / MAX_FAN_RPM * 100.0;
    double load_r = round(load / 5.0) * 5.0;
    sprintf(icon_name, "brasero-disc-%02d", (int) load_r);
    app_indicator_set_icon_full(indicator, icon_name, "Clevo Fan Indicator");
    return G_SOURCE_CONTINUE;
}

static void ui_command_set_fan(long fan_duty) {
    
    int fan_duty_val = (int) fan_duty;
    if (fan_duty_val == 0) {
        printf("clicked on fan duty auto\n");

    }
    else if(fan_duty_val > 0){
        double index = (double) (fan_duty_val/10)+startIndex;
        printf("index: %lf\n", index);
        ui_toggle_menuitems(index);
        share_info->auto_duty = 0;
        share_info->auto_duty_val = 0;
        share_info->manual_next_fan_duty = fan_duty_val;
        gtk_widget_set_sensitive(menuitems[(int)index].widget, false);
        gtk_widget_set_sensitive(menuitems[0].widget, true);
        gtk_widget_set_sensitive(menuitems[1].widget, true);
    }
    else if(fan_duty_val == -2){
        double index = (double) (int)(10/10)+startIndex;
        ui_toggle_menuitems(index);
        share_info->auto_duty = 1;
        share_info->auto_duty_val = 0;
        share_info->manual_next_fan_duty = 0;
        share_info->performance_mode = 1; 
        gtk_widget_set_sensitive(menuitems[0].widget, false);
        gtk_widget_set_sensitive(menuitems[1].widget, true);
        
    }
    else if (fan_duty_val == -1){
        double index = (double) (int)(10/10)+startIndex;
        ui_toggle_menuitems(index);
        share_info->auto_duty = 1;
        share_info->auto_duty_val = 0;
        share_info->manual_next_fan_duty = 0;   
        share_info->performance_mode = 0;
        gtk_widget_set_sensitive(menuitems[0].widget, true);
        gtk_widget_set_sensitive(menuitems[1].widget, false);
    }   
}

static void ui_command_quit(gchar* command) {
    printf("clicked on quit\n");
    gtk_main_quit();
}

static void ui_toggle_menuitems(int fan_duty) 
{
    for (int i = 0; i < menuitem_count; i++) 
    {
        if (menuitems[i].widget == NULL)
            continue;
        if (fan_duty == 0)
            gtk_widget_set_sensitive(menuitems[i].widget, menuitems[i].type == AUTO);
        else
            gtk_widget_set_sensitive(menuitems[i].widget, 
            (menuitems[i].type == MANUAL || menuitems[i].type == NA) && !((int)menuitems[fan_duty].option == fan_duty*10));
    }  


}

static int ec_init(void) {
    if (ioperm(EC_DATA, 1, 1) != 0)
        return EXIT_FAILURE;
    if (ioperm(EC_SC, 1, 1) != 0)
        return EXIT_FAILURE;
    return EXIT_SUCCESS;
}

static void ec_on_sigterm(int signum) {
    printf("ec on signal: %s\n", strsignal(signum));
    if (share_info != NULL)
        share_info->exit = 1;
}

static int ec_auto_duty_adjust(void) {
    int temp = MAX(share_info->cpu_temp, share_info->gpu_temp);
    for(unsigned int i = 0; i < share_info->nvidia_devices[0].device_count;i++)
        temp = MAX(temp, share_info->nvidia_devices[i].gpu_temp);
    //printf("temp: %d cpu1: %d cpu2: %d gpu1: %d gpu2: %d\n",  temp,a->cpu_temp, a->gpu_temp, a->nvidia_devices[0].gpu_temp, a->nvidia_devices[1].gpu_temp);
    int duty = share_info->fan_duty;
    //
    if(share_info->performance_mode) 
    {
    if (temp >= 78 && duty < 100)
        return 100;
    if (temp >= 70 && duty < 90)
        return 90;
    if (temp >= 60 && duty < 80)
        return 80;
    if (temp >= 50 && duty < 70)
        return 70;
    if (temp >= 40 && duty < 60)
        return 60;
    if (temp >= 30 && duty < 50)
        return 50;
    if (temp >= 20 && duty < 40)
        return 40;
    if (temp >= 10 && duty < 30)
        return 30;
    //
    if (temp <= 15 && duty > 30)
        return 30;
    if (temp <= 25 && duty > 40)
        return 40;
    if (temp <= 35 && duty > 50)
        return 50;
    if (temp <= 45 && duty > 60)
        return 60;
    if (temp <= 55 && duty > 70)
        return 70;
    if (temp <= 65 && duty > 80)
        return 80;
    if (temp <= 75 && duty > 90)
        return 90;
    } else 
    {
    if (temp >= 80 && duty < 50)
        return 85;
    if (temp >= 70 && duty < 40)
        return 40;
    if (temp >= 60 && duty < 30)
        return 30;
    if (temp >= 55 && duty < 25)
        return 25;
    if (temp >= 40 && duty < 20)
        return 20;
    if (temp >= 30 && duty < 10)
        return 10;
    //
    if (temp <= 15 && duty > 0)
        return 0;
    if (temp <= 25 && duty > 10)
        return 10;
    if (temp <= 35 && duty > 15)
        return 15;
    if (temp <= 45 && duty > 20)
        return 20;
    if (temp <= 55 && duty > 25)
        return 25;
    if (temp <= 65 && duty > 30)
        return 30;
    if (temp <= 75 && duty > 50)
        return 50;}
    //
    return 0;
}

static int ec_query_cpu_temp(void) {
    return ec_io_read(EC_REG_CPU_TEMP);
}

static int ec_query_gpu_temp(void) {
    return ec_io_read(EC_REG_GPU_TEMP);
}

static int ec_query_fan_duty(int fan_number) {
    int raw_duty;
    if (fan_number == 2) {
        raw_duty = ec_io_read(EC_REG_FA2_DUTY);
    }
    else {
        raw_duty = ec_io_read(EC_REG_FAN_DUTY);
    }
    return calculate_fan_duty(raw_duty);
}

static int ec_query_fan_rpms(void) {
    int raw_rpm_hi = ec_io_read(EC_REG_FAN_RPMS_HI);
    int raw_rpm_lo = ec_io_read(EC_REG_FAN_RPMS_LO);
    return calculate_fan_rpms(raw_rpm_hi, raw_rpm_lo);
}

static int ec_query_fa2_rpms(void) {
    int raw_rpm_hi = ec_io_read(EC_REG_FA2_RPMS_HI);
    int raw_rpm_lo = ec_io_read(EC_REG_FA2_RPMS_LO);
    return calculate_fan_rpms(raw_rpm_hi, raw_rpm_lo);
}

static int ec_write_fan_duty(int duty_percentage, int fan_number) {
    if (duty_percentage < 0 || duty_percentage > 100) {
        printf("Wrong fan duty to write: %d\n", duty_percentage);
        return EXIT_FAILURE;
    }
    double v_d = ((double) duty_percentage) / 100.0 * 255.0;
    int v_i = (int) v_d;

    return ec_io_do(0x99, fan_number, v_i);
}

static int dump_ec(void)
{
#define BUF_SIZE 256
#define HIST_SIZE 16384
    printf("setting fan table to vtable\n");
    int io_fd = open("/sys/kernel/debug/ec/ec0/io", O_RDONLY, 0);
    if (io_fd < 0) {
            printf("unable to read EC from sysfs: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
    }
    unsigned char buf[BUF_SIZE];
    
    ssize_t len = read(io_fd, buf, BUF_SIZE);
    switch (len) 
    {
        case -1:
            printf("unable to read EC from sysfs: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        default:
        for(int i = 0; i < BUF_SIZE; i++) if(i%16==0){printf("\n%02x(%03d):\e[33m%02x\e[0m ",i,i,ec_io_read(i));}else printf("%02x(%03d):\e[33m%02x\e[0m ",i,i,ec_io_read(i));
        printf("\n");
    }
    close(io_fd);
    return EXIT_SUCCESS;
}

static int ec_set_use_vfan_chart(void) 
{
    // sys_fs only provides 3 fan access get/set point according to /sys/kernel/debug/ec/ec0
    return EXIT_SUCCESS;//ec_io_do(0x99, 7, 0xFF);
}

static int ec_io_wait(const uint32_t port, const uint32_t flag,
        const char value) {
    uint8_t data = inb(port);
    int i = 0;
    while ((((data >> flag) & 0x1) != value) && (i++ < 100)) {
        usleep(1000);
        data = inb(port);
        //printf("data: %d port: %d\n", data, port);
    }
    if (i >= 100) {
        printf("wait_ec error on port 0x%x, data=0x%x, flag=0x%x, value=0x%x\n",
                port, data, flag, value);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static uint8_t ec_io_read(const uint32_t port) {
    ec_io_wait(EC_SC, IBF, 0);
    outb(EC_SC_READ_CMD, EC_SC);

    ec_io_wait(EC_SC, IBF, 0);
    outb(port, EC_DATA);

    //wait_ec(EC_SC, EC_SC_IBF_FREE);
    ec_io_wait(EC_SC, OBF, 1);
    uint8_t value = inb(EC_DATA);

    return value;
}

static int ec_io_do(const uint32_t cmd, const uint32_t port,
        const uint8_t value) {
    ec_io_wait(EC_SC, IBF, 0);
    outb(cmd, EC_SC);

    ec_io_wait(EC_SC, IBF, 0);
    outb(port, EC_DATA);

    ec_io_wait(EC_SC, IBF, 0);
    outb(value, EC_DATA);

    return ec_io_wait(EC_SC, IBF, 0);
}

static int calculate_fan_duty(int raw_duty) {
    return (int) ((double) raw_duty / 255.0 * 100.0);
}

static int calculate_fan_rpms(int raw_rpm_high, int raw_rpm_low) {
    int raw_rpm = (raw_rpm_high << 8) + raw_rpm_low;
    return raw_rpm > 0 ? (2156220 / raw_rpm) : 0;
}

static int check_proc_instances(const char* proc_name) {
    int proc_name_len = strlen(proc_name);
    pid_t this_pid = getpid();
    DIR* dir;
    if (!(dir = opendir("/proc"))) {
        perror("can't open /proc");
        return -1;
    }
    int instance_count = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        char* endptr;
        long lpid = strtol(ent->d_name, &endptr, 10);
        if (*endptr != '\0')
            continue;
        if (lpid == this_pid)
            continue;
        char buf[512];
        snprintf(buf, sizeof(buf), "/proc/%ld/comm", lpid);
        FILE* fp = fopen(buf, "r");
        if (fp) {
            if (fgets(buf, sizeof(buf), fp) != NULL) {
                if ((buf[proc_name_len] == '\n' || buf[proc_name_len] == '\0')
                        && strncmp(buf, proc_name, proc_name_len) == 0) {
                    fprintf(stderr, "Process: %ld\n", lpid);
                    instance_count += 1;
                }
            }
            fclose(fp);
        }
    }
    closedir(dir);
    return instance_count;
}

static void get_time_string(char* buffer, size_t max, const char* format) {
    time_t timer;
    struct tm tm_info;
    time(&timer);
    localtime_r(&timer, &tm_info);
    strftime(buffer, max, format, &tm_info);
}

static void signal_term(__sighandler_t handler) {
    signal(SIGHUP, handler);
    signal(SIGINT, handler);
    signal(SIGQUIT, handler);
    signal(SIGPIPE, handler);
    signal(SIGALRM, handler);
    signal(SIGTERM, handler);
    signal(SIGUSR1, handler);
    signal(SIGUSR2, handler);
}
