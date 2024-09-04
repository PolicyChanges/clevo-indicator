from cProfile import label
from matplotlib import pyplot as plt
import math

def ec_auto_duty_adjust(temp, duty):
    if (temp >= 80 and duty < 100):
        return 100
    if (temp >= 70 and duty < 60):
        return 50
    if (temp >= 65 and duty < 40):
        return 20
    if (temp >= 50 and duty < 20):
        return 10

    if (temp <= 45 and duty > 0):
        return 0
    if (temp <= 55 and duty > 20):
        return 20
    if (temp <= 65 and duty > 40):
        return 40
    if (temp <= 75 and duty > 60):
        return 60
    return -1


def fan_curve(temp, duty):
    return min(max((temp/5)*10 - 60, 0), 100)

def sigmoid_curve(temp, duty, x50L=50, x50U=60):
    a = (x50L + x50U) / 2
    b = 2 / abs(x50L - x50U)
    return 1/(1 + math.exp(b/2 * -(temp-(a+10)))) * 100  #* min(max((temp/5)*10 - 60, 0), 100)
    #return 1/(1 + math.exp(b * -(temp-a))) * 100
curve = []
sig_curve = []
old = []
for i in range(100):
    old.append(ec_auto_duty_adjust(i, 0))
    curve.append(fan_curve(i, 0))
    sig_curve.append(sigmoid_curve(i, 0))

plt.figure()
# plt.plot(curve, label="curve")
plt.plot(sig_curve, label="sigmoid")
# plt.plot(old, label="old")
plt.legend()
plt.show()

# TODO: Implement PID control for this
