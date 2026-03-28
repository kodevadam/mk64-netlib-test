#include "libultra_internal.h"
#include "hardware.h"

extern u32 osRomBase;
extern void __osPiGetAccess(void);
extern void __osPiRelAccess(void);

s32 osPiWriteIo(u32 devAddr, u32 data) {
    register int status;
    __osPiGetAccess();
    status = HW_REG(PI_STATUS_REG, u32);
    while (status & (PI_STATUS_BUSY | PI_STATUS_IOBUSY | PI_STATUS_ERROR)) {
        status = HW_REG(PI_STATUS_REG, u32);
    }
    HW_REG(osRomBase | devAddr, u32) = data;
    __osPiRelAccess();
    return 0;
}
