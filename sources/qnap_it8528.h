#ifndef HW_MISC_QNAP_IT8528_H
#define HW_MISC_QNAP_IT8528_H

#include "qemu/osdep.h"
#include "monitor/monitor.h"

#define TYPE_QNAP_IT8528           "qnap-it8528"

void qnap_it8528_hmp_info(Monitor *mon, const QDict *qdict);
void qnap_it8528_hmp_press(Monitor *mon, QDict *qdict);

#endif 
