

`hmp-commands.hx`:
```
{
    .name      = "qnap_it8528_info",
    .args_type = "",
    .params    = "",
    .help      = "show QNAP IT8528 EC state",
    .cmd       = qnap_it8528_hmp_info,
},
{
    .name      = "qnap_it8528_press",
    .args_type = "button:s,duration:i",
    .params    = "button duration_ms",
    .help      = "simulate QNAP IT8528 button press (CHASSIS|COPY|RESET)",
    .cmd       = qnap_it8528_hmp_press,
},
```

`hw/misc/Kconfig`
```
config QNAP_IT8528
    bool
    default y
    depends on ISA_BU
```

`hw/misc/meson.build`
```
system_ss.add(when: 'CONFIG_QNAP_IT8528', if_true: files('qnap_it8528.c'))
```

`monitor/hmp-target.c`
```
#include "hw/misc/qnap_it8528.h"
```