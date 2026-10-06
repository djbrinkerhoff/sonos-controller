#include "crash.hpp"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_private/panic_internal.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "riscv/rvruntime-frames.h"
#include <cstring>

// A panic report goes to the USB serial console only, and the log ring
// starts over on reboot, so a crash with no cable attached left nothing to
// read. The linker routes IDF's panic handler through the wrapper below
// (-Wl,--wrap=esp_panic_handler in CMakeLists.txt), which copies the essentials
// into RTC memory that survives the restart; crash_report() logs them on the
// next boot. Decode the addresses with:
//   riscv32-esp-elf-addr2line -pfiaC -e build/sonos_controller.elf <address>...
// against the ELF of the image that crashed.
namespace {
const char* TAG="crash";
constexpr uint32_t MAGIC=0x43524153;  // "CRAS"
constexpr int STACK_WORDS=64;
struct Record {
    uint32_t magic;
    uint32_t mepc, ra, sp, mcause, mtval, uptime_s;
    char reason[64];
    char task[16];
    uint32_t stack[STACK_WORDS];
    uint32_t reported;
};
RTC_NOINIT_ATTR Record record;
}

extern "C" void __real_esp_panic_handler(panic_info_t* info);
extern "C" void __wrap_esp_panic_handler(panic_info_t* info) {
    record.magic=0;  // invalid until complete: a crash inside here leaves no half record
    const auto* frame=static_cast<const RvExcFrame*>(info->frame);
    if(frame) {
        record.mepc=frame->mepc; record.ra=frame->ra; record.sp=frame->sp;
        record.mcause=frame->mcause; record.mtval=frame->mtval;
        const auto* sp=reinterpret_cast<const uint32_t*>(frame->sp);
        for(int i=0;i<STACK_WORDS;++i)
            record.stack[i]=esp_ptr_byte_accessible(sp+i)?sp[i]:0;
    }
    record.uptime_s=static_cast<uint32_t>(esp_timer_get_time()/1000000);
    record.reason[0]=0;
    if(info->reason) strlcpy(record.reason,info->reason,sizeof record.reason);
    record.task[0]=0;
    if(auto task=xTaskGetCurrentTaskHandleForCore(info->core)) strlcpy(record.task,pcTaskGetName(task),sizeof record.task);
    record.reported=0;
    record.magic=MAGIC;
    __real_esp_panic_handler(info);
}

void crash_report() {
    if(record.magic!=MAGIC || record.reported) return;
    record.reported=1;
    ESP_LOGE(TAG,"Previous run panicked after %lu s in task '%s': %s",static_cast<unsigned long>(record.uptime_s),record.task,record.reason);
    ESP_LOGE(TAG,"mepc 0x%08lx  ra 0x%08lx  sp 0x%08lx  mcause 0x%lx  mtval 0x%08lx",
             static_cast<unsigned long>(record.mepc),static_cast<unsigned long>(record.ra),static_cast<unsigned long>(record.sp),
             static_cast<unsigned long>(record.mcause),static_cast<unsigned long>(record.mtval));
    // Code addresses found on the stack: the likely callers, innermost first.
    char line[200]; int used=0, found=0;
    for(int i=0;i<STACK_WORDS && found<16;++i) {
        if(!esp_ptr_executable(reinterpret_cast<void*>(record.stack[i]))) continue;
        used+=snprintf(line+used,sizeof line-used," 0x%08lx",static_cast<unsigned long>(record.stack[i]));
        ++found;
        if(used>static_cast<int>(sizeof line)-12) break;
    }
    if(found) ESP_LOGE(TAG,"stack code addresses:%s",line);
}
