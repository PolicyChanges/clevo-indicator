/*
 ============================================================================
 Name        : clevo-indicator.c
 Author      : AqD <iiiaqd@gmail.com>
 Version     :
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

#include <libayatana-appindicator/app-indicator.h>

#include  "nvml.h"

#define NAME "clevo-indicator"

#define EC_SC 0x66
#define EC_DATA 0x62

#define IBF 1
#define OBF 0
#define EC_SC_READ_CMD 0x80

#define FAN_1 0x01
#define FAN_2 0x02


/* EC registers can be read by EC_SC_READ_CMD or /sys/kernel/debug/ec/ec0/io:
 *
 * 1.   
 * 2. od -Ax -t x1 /sys/kernel/debug/ec/ec0/io
 */

#define EC_REG_SIZE 0x100
#define EC_REG_CPU_TEMP 0x07
#define EC_REG_GPU_TEMP 0xCD
#define EC_REG_CPU_FAN_DUTY 0xCE
#define EC_REG_GPU_FAN_DUTY 0xCF
#define EC_REG_FAN_1_RPMS_HI 0xD0
#define EC_REG_FAN_1_RPMS_LO 0xD1
#define EC_REG_FAN_2_RPMS_HI 0xD2
#define EC_REG_FAN_2_RPMS_LO 0xD3

#define MAX_FAN_RPM 4400.0

static char* help_text = "\n\
Usage: clevo-indicator [fan-duty-percentage]\n\
\n\
Dump/Control fan duty on Clevo laptops. Display indicator by default.\n\
\n\
Arguments:\n\
  [fan-duty-percentage]\t\tTarget fan duty in percentage, from 40 to 100\n\
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
\n";

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
static void ui_command_quit(gchar* command);
static void ui_toggle_menuitems(int fan_duty);
static void ec_on_sigterm(int signum);
static int ec_init(void);
static int ec_query_cpu_temp(void);
static int ec_query_gpu_temp(void);
static int ec_query_fan_duty(const uint32_t reg);
static int ec_query_fan_rpms(int fan);
static int ec_write_fan_duty(int duty_percentage);
static int ec_io_wait(const uint32_t port, const uint32_t flag,
        const char value);
static uint8_t ec_io_read(const uint32_t port);
static int ec_io_do(const uint32_t cmd, const uint32_t port,
        const uint8_t value);
static int calculate_fan_duty(int raw_duty);
static int calculate_fan_rpms(int raw_rpm_high, int raw_rpm_low);
static int check_proc_instances(const char* proc_name);
static void get_time_string(char* buffer, size_t max, const char* format);
static void signal_term(__sighandler_t handler);

typedef struct nvidia_device_t {
	nvmlDevice_t device;
	nvmlTemperatureSensors_t sensor;// NVML_TEMPERATURE_GPU
	char name[NVML_DEVICE_NAME_BUFFER_SIZE];
	unsigned int gpu_temp;

}nvidia_device;

nvidia_device *init_nvml(nvidia_device*, unsigned int *);
static int ec_auto_duty_adjust(nvidia_device *);

static AppIndicator* indicator = NULL;

struct {
    char label[256];
    GCallback callback;
    long option;
    MenuItemType type;
    GtkWidget* widget;

}

static menuitems[] = {
        { "Set FAN to AUTO", G_CALLBACK(ui_command_set_fan), 0, AUTO, NULL },
        { "", NULL, 0L, NA, NULL },
        { "Set FAN to  0%", G_CALLBACK(ui_command_set_fan), 0, MANUAL, NULL },
        { "Set FAN to  10%", G_CALLBACK(ui_command_set_fan), 10, MANUAL, NULL },
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

static int menuitem_count = (sizeof(menuitems) / sizeof(menuitems[0]));

struct {
    volatile int exit;
    volatile int cpu_temp;
    volatile int gpu_temp;
    volatile int gpu_temp2;
    volatile int cpu_fan_duty;
    volatile int gpu_fan_duty;
    volatile int fan_1_rpms;
    volatile int fan_2_rpms;
    volatile int auto_duty;
    volatile int auto_duty_val;
    volatile int manual_next_fan_duty;
    volatile int manual_prev_fan_duty;
}static *share_info = NULL;

static pid_t parent_pid = 0;

int main(int argc, char* argv[]) {
    printf("Simple fan control utility for Clevo laptops\n");
    //init_nvml();
    if (check_proc_instances(NAME) > 1) {
        printf("Multiple running instances!\n");
        char* display = getenv("DISPLAY");
        if (display != NULL && strlen(display) > 0) {
            int desktop_uid = getuid();
            setuid(desktop_uid);
            //
            gtk_init(&argc, &argv);
            GtkWidget* dialog = gtk_message_dialog_new(NULL, 0,
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
        if (argv[1][0] == '-') {
            printf(help_text);
            return main_dump_fan();
        } else {
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
    share_info = shm;
    share_info->exit = 0;
    share_info->cpu_temp = 0;
    share_info->gpu_temp = 0;
    share_info->gpu_temp2 = 0;
    share_info->gpu_fan_duty = 0;
    share_info->fan_2_rpms = 0;
    share_info->cpu_fan_duty = 0;
    share_info->fan_1_rpms = 0;
    share_info->auto_duty = 1;
    share_info->auto_duty_val = -1;
    share_info->manual_next_fan_duty = 0;
    share_info->manual_prev_fan_duty = 0;
}

static int main_ec_worker(void) {
    setuid(0);
    system("modprobe ec_sys");
    
    nvidia_device *nvidia_devices = NULL;
    unsigned int nvidia_device_count = 0;

    nvidia_devices = init_nvml(nvidia_devices, &nvidia_device_count);
	
	//FILE *io_file = fopen("/sys/kernel/debug/ec/ec0/io", O_RDONLY);
	/*
	if (io_file < 0) {
		printf("unable to read EC from sysfs: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}*/
	
    while (share_info->exit == 0) {
		int sleep_interval = 200;
        // check parent
        if (parent_pid != 0 && kill(parent_pid, 0) == -1) {
            printf("worker on parent death\n");
            break;
        }
		// read EC  -- TODO: must be a better way
		int io_fd = open("/sys/kernel/debug/ec/ec0/io", O_RDONLY, 0);
		if (io_fd < 0) {
			printf("unable to read EC from sysfs: %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		}
			// write EC
		int new_fan_duty = share_info->manual_next_fan_duty;
		if (new_fan_duty != 0
				&& new_fan_duty != share_info->manual_prev_fan_duty) {
			ec_write_fan_duty(new_fan_duty);
			share_info->manual_prev_fan_duty = new_fan_duty;
		}
		
        unsigned char buf[EC_REG_SIZE];
        ssize_t len = read(io_fd, buf, EC_REG_SIZE);
        //size_t len = fread(buf, sizeof(unsigned char), EC_REG_SIZE, io_file);
        
        switch (len) {
        case -1:
            printf("unable to read EC from sysfs: %s\n", strerror(errno));
            break;
        case 0x100:
            share_info->cpu_temp = buf[EC_REG_CPU_TEMP];
            share_info->gpu_temp = nvml_query_gpu_temp(nvidia_devices[0]);
            share_info->gpu_temp2 = nvml_query_gpu_temp(nvidia_devices[1]);
            //get_gpu_temperature(&share_info->gpu_temp, &share_info->gpu_temp2);
            //printf("GPU1=%d, GPU2=%d\n", nvml_query_gpu_temp(nvidia_devices[0]), nvml_query_gpu_temp(nvidia_devices[1]));
            share_info->cpu_fan_duty = calculate_fan_duty(buf[EC_REG_CPU_FAN_DUTY]);
            share_info->gpu_fan_duty = calculate_fan_duty(buf[EC_REG_GPU_FAN_DUTY]);
            share_info->fan_1_rpms = calculate_fan_rpms(buf[EC_REG_FAN_1_RPMS_HI],
                    buf[EC_REG_FAN_1_RPMS_LO]);
            share_info->fan_2_rpms = calculate_fan_rpms(buf[EC_REG_FAN_2_RPMS_HI],
                    buf[EC_REG_FAN_2_RPMS_LO]);
            
            //printf("ndevices: %d\n", nvidia_device_count);
            
			//printf("temp=%d, duty=%d, rpms=%d\n", share_info->cpu_temp,
			//share_info->fan_duty, share_info->fan_rpms);
             
            break;
        default:
            printf("wrong EC size from sysfs: %ld\n", len);
        }
        
        close(io_fd);
        
        int next_duty = 100;
        // auto EC
        if (share_info->auto_duty == 1) {
            next_duty = ec_auto_duty_adjust(nvidia_devices);
            
            next_duty = (int)(ceil((float)next_duty / 10.0) * 10);      
                 
            if (next_duty != -1 && next_duty != share_info->auto_duty_val) {
                char s_time[256];
                
                get_time_string(s_time, 256, "%d/%m %H:%M:%S");
                printf("%s CPU=%d°C, GPU1=%d°C, GPU2=%d°C auto fan duty to %d%%\n", s_time,
                        share_info->cpu_temp, share_info->gpu_temp, share_info->gpu_temp2, next_duty);
                ec_write_fan_duty(next_duty);
                share_info->auto_duty_val = next_duty;
            }
        }
        
	   if(share_info->auto_duty_val > next_duty) sleep_interval = 8000; 
		usleep(sleep_interval * 1000);
    }
    
    //fclose(io_file);
	if(nvidia_devices != NULL)
		free(nvidia_devices);
	
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
                    G_CALLBACK(menuitems[i].callback),
                    (void* ) menuitems[i].option);
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(indicator_menu), item);
        menuitems[i].widget = item;
    }
    gtk_widget_show_all(indicator_menu);
    //
    indicator = app_indicator_new(NAME, "brasero",
            APP_INDICATOR_CATEGORY_HARDWARE);
    g_assert(IS_APP_INDICATOR(indicator));
    app_indicator_set_label(indicator, "Init..", "XX");
    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE); //APP_INDICATOR_STATUS_ATTENTION -- only works in gnome
    app_indicator_set_ordering_index(indicator, -2);
    app_indicator_set_title(indicator, "Clevo");
    app_indicator_set_menu(indicator, GTK_MENU(indicator_menu));
    g_timeout_add(1000, &ui_update, NULL);
    ui_toggle_menuitems(share_info->cpu_fan_duty);
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
    printf("  CPU FAN Duty: %d%%\n", ec_query_fan_duty(EC_REG_CPU_FAN_DUTY));
    printf("  GPU FAN Duty: %d%%\n", ec_query_fan_duty(EC_REG_GPU_FAN_DUTY));
    printf("  CPU FAN RPMs: %d RPM\n", ec_query_fan_rpms(1));
    printf("  GPU FAN RPMs: %d RPM\n", ec_query_fan_rpms(2));
    printf("  CPU Temp: %d°C\n", ec_query_cpu_temp());
    //printf("  GPU 1 %d Temp: %d°C\n", i+1, nvml_query_gpu_temp(i));
    return EXIT_SUCCESS;
}

static int main_test_fan(int duty_percentage) {
    printf("Change fan duty to %d%%\n", duty_percentage);
    ec_write_fan_duty(duty_percentage);
    printf("\n");
    main_dump_fan();
    return EXIT_SUCCESS;
}

static gboolean ui_update(gpointer user_data) {
    char label[256];
    sprintf(label, "CPU: %d℃ GPU: %d℃", share_info->cpu_temp, share_info->gpu_temp);
    app_indicator_set_label(indicator, label, "XXXXXX");
    char icon_name[256];
    double load = ((double) share_info->fan_1_rpms) / MAX_FAN_RPM * 100.0;
    double load_r = round(load / 5.0) * 5.0;
    sprintf(icon_name, "brasero-disc-%02d", (int) load_r);
    app_indicator_set_icon(indicator, icon_name);
    return G_SOURCE_CONTINUE;
}

static void ui_command_set_fan(long fan_duty) {
    int fan_duty_val = (int) fan_duty;
    if (fan_duty_val == 0) {
        printf("clicked on fan duty auto\n");
        share_info->auto_duty = 1;
        share_info->auto_duty_val = -1;
        share_info->manual_next_fan_duty = 0;
    } else {
        printf("clicked on fan duty: %d\n", fan_duty_val);
        share_info->auto_duty = 0;
        share_info->auto_duty_val = -1;
        share_info->manual_next_fan_duty = fan_duty_val;
    }
    ui_toggle_menuitems(fan_duty_val);
}

static void ui_command_quit(gchar* command) {
    printf("clicked on quit\n");
    gtk_main_quit();
}

static void ui_toggle_menuitems(int fan_duty) {
    for (int i = 0; i < menuitem_count; i++) {
        if (menuitems[i].widget == NULL)
            continue;
        if (fan_duty == 0)
            gtk_widget_set_sensitive(menuitems[i].widget,
                    menuitems[i].type != AUTO);
        else
            gtk_widget_set_sensitive(menuitems[i].widget,
                    menuitems[i].type != MANUAL
                            || (int) menuitems[i].option != fan_duty);
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

int ec_auto_duty_adjust(nvidia_device *nvidia_devices) {
    share_info->gpu_temp = nvml_query_gpu_temp(nvidia_devices[0]);
    share_info->gpu_temp2 = nvml_query_gpu_temp(nvidia_devices[1]);

    int temp = MAX(MAX(share_info->cpu_temp, share_info->gpu_temp), share_info->gpu_temp2);
    int duty = share_info->cpu_fan_duty;
	//printf("max temp: %d\n", temp);
    const double EULER = 2.71828182845904523536;
    double x50L = 50.0;
    double x50U = 60.0;
    double a = (x50L + x50U) / 2;
    double b = 2.0 / abs(x50L - x50U);
    // printf("b: %lf\n", b);
    // printf("temp: %d\n", temp);
    double pre_prod = (b * -(temp-a));
    // printf("pre_prod: %lf\n", pre_prod);
    double power = pow(EULER, pre_prod);
    // printf("power: %lf\n", power);
    double val = 1/(1 + power) * 100;
    //printf("ret: %lf\n", val);
    if(val < 40) val=40;
    return val;

}

static int ec_query_cpu_temp(void) {
    return ec_io_read(EC_REG_CPU_TEMP);
}

int nvml_query_gpu_temp(nvidia_device device) {
    nvmlDeviceGetTemperature(device.device, device.sensor, &device.gpu_temp);
    return device.gpu_temp;
}

// static int ec_query_gpu_temp(void) {
//     return ec_io_read(EC_REG_GPU_TEMP);
// }

static int ec_query_fan_duty(const uint32_t reg) {
    int raw_duty = ec_io_read(reg);
    return calculate_fan_duty(raw_duty);
}

static int ec_query_fan_rpms(int fan) {
    if(fan == 1){
        int raw_rpm_hi = ec_io_read(EC_REG_FAN_1_RPMS_HI);
        int raw_rpm_lo = ec_io_read(EC_REG_FAN_1_RPMS_LO);
        return calculate_fan_rpms(raw_rpm_hi, raw_rpm_lo);
    }
    else{
        int raw_rpm_hi = ec_io_read(EC_REG_FAN_2_RPMS_HI);
        int raw_rpm_lo = ec_io_read(EC_REG_FAN_2_RPMS_LO);
        return calculate_fan_rpms(raw_rpm_hi, raw_rpm_lo);
    }
}

static int ec_write_fan_duty(int duty_percentage) {
    if (duty_percentage < 0 || duty_percentage > 100) {
        printf("Wrong fan duty to write: %d\n", duty_percentage);
        return EXIT_FAILURE;
    }
    double v_d = ((double) duty_percentage) / 100.0 * 255.0;
    int v_i = (int) v_d;
    ec_io_do(0x99, FAN_2, v_i);
    return ec_io_do(0x99, FAN_1, v_i);
}

static int ec_io_wait(const uint32_t port, const uint32_t flag,
        const char value) {
    uint8_t data = inb(port);
    int i = 0;
    while ((((data >> flag) & 0x1) != value) && (i++ < 100)) {
        usleep(1000);
        data = inb(port);
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


nvidia_device *init_nvml(nvidia_device *nvidia_devices, unsigned int *nvidia_device_count){
    nvmlReturn_t result;
    
    // First initialize NVML library
    result = nvmlInit();
    if (NVML_SUCCESS != result)
    { 
        printf("Failed to initialize NVML: %s\n", nvmlErrorString(result));

        printf("Press ENTER to continue...\n");
        getchar();
        return;
    }

    result = nvmlDeviceGetCount(nvidia_device_count);
    
	nvidia_devices = (nvidia_device*)malloc(sizeof(nvidia_device) * *nvidia_device_count);

    if (NVML_SUCCESS != result)
    { 
        printf("Failed to query device count: %s\n", nvmlErrorString(result));
        return;
    }
    
    printf("Found %u device%s\n\n", *nvidia_device_count, *nvidia_device_count != 1 ? "s" : "");
    printf("Listing devices:\n");
    
    nvmlPciInfo_t pci;
    // nvmlComputeMode_t compute_mode;

    // Query for device handle to perform operations on a device
    // You can also query device handle by other features like:
    // nvmlDeviceGetHandleBySerial
    // nvmlDeviceGetHandleByPciBusId
    for(int i = 0; i < *nvidia_device_count; i++){
		nvidia_devices[i].sensor = NVML_TEMPERATURE_GPU;
		result = nvmlDeviceGetHandleByIndex(0, &nvidia_devices[i].device);
		result = nvmlDeviceGetName(nvidia_devices[i].device, nvidia_devices[i].name, NVML_DEVICE_NAME_BUFFER_SIZE);
		result = nvmlDeviceGetPciInfo(nvidia_devices[i].device, &pci);
		printf("%u. %s [%s]\n", 0, nvidia_devices[i].name, pci.busId);
		nvmlDeviceGetTemperature(nvidia_devices[i].device, nvidia_devices[i].sensor, &nvidia_devices[i].gpu_temp);
		printf("TEMP NV: %d\n", nvidia_devices[i].gpu_temp);
	}
	return nvidia_devices;
}
