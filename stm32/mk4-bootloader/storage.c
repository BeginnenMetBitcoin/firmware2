// (c) Copyright 2018 by Coinkite Inc. This file is covered by license found in COPYING-CC.
// 
// storage.c -- manage flash and its sensitive contents.
//
// NOTE: ST's flash is different from others: it has ECC in effect, and can only
// be programmed once. Writing ones is included in that. I'm used to flash that
// can go from 1 to 0 anytime. One exception is you can write (64-bit) zero into flash
// that is not erased. Consequence is if DFU file includes a page we want to write
// later, we would need to erase it first. Not clear if DFU can do a bulk erase.
//
//
#include "basics.h"
#include "storage.h"
#include "rng.h"
#include "oled.h"
#include "ae.h"
#include "se2.h"
#include "console.h"
#include "faster_sha256.h"
#include <string.h>
#include <errno.h>
#include "assets/screens.h"
#include "stm32l4xx_hal.h"
#include "constant_time.h"

#if defined (STM32L4P5xx) || defined (STM32L4Q5xx) || defined (STM32L4R5xx) || defined (STM32L4R7xx) || defined (STM32L4R9xx) || defined (STM32L4S5xx) || defined (STM32L4S7xx) || defined (STM32L4S9xx)
#define FLASH_NB_DOUBLE_WORDS_IN_ROW  64
#else
#define FLASH_NB_DOUBLE_WORDS_IN_ROW  32
#endif

// Number of flash pages to write-protect (ie. our size in flash pages)
// - written into FLASH->WRP1AR
// - once done, can't change bootrom via DFU (no error given, but doesn't change)
const uint32_t num_pages_locked = ((BL_FLASH_SIZE + BL_NVROM_SIZE) / FLASH_PAGE_SIZE)-1; // == 15

// flash_setup0()
//
    void
flash_setup0(void)
{
    // PROBLEM: we are running in bank 1 (of 2) and want to program
    // bits in the same bank. Cannot read bank while programming it.
    // Therefore, must have our programming code running in RAM.

    // put the ram-callable functions into place
    extern uint8_t _srelocate, _etext, _erelocate;
    memcpy(&_srelocate, &_etext, ((uint32_t)&_erelocate)-(uint32_t)&_srelocate);

    // turn on clock to flash registers
    __HAL_RCC_FLASH_CLK_ENABLE();
}

// _flash_wait_done()
//
// Like FLASH_WaitForLastOperation((uint32_t)FLASH_TIMEOUT_VALUE)
// Absolutely MUST be in RAM.
//
    __attribute__((section(".ramfunc")))
    __attribute__((always_inline))
    static inline uint32_t
_flash_wait_done(void)
{
    while(__HAL_FLASH_GET_FLAG(FLASH_FLAG_BSY)) {
        // busy wait
    }

    uint32_t error = (FLASH->SR & FLASH_FLAG_SR_ERRORS);
    if(error) {
        // Save an error code; somewhat random, depends on chip details
        return error;
    }

    // Check FLASH End of Operation flag
    if (__HAL_FLASH_GET_FLAG(FLASH_FLAG_EOP)) {
        // Clear FLASH End of Operation pending bit
        __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP);
    }

    return 0;
}

// flash_lock()
//
// Ok to run from flash.
//
    void
flash_lock(void)
{
    // see HAL_FLASH_Lock();
    SET_BIT(FLASH->CR, FLASH_CR_LOCK);
}

// flash_unlock()
//
// Ok to run from flash.
//
    void
flash_unlock(void)
{
    // see HAL_FLASH_Unlock();
    if(READ_BIT(FLASH->CR, FLASH_CR_LOCK)) {
        // Authorize the FLASH Registers access
        WRITE_REG(FLASH->KEYR, FLASH_KEY1);
        WRITE_REG(FLASH->KEYR, FLASH_KEY2);

        if(READ_BIT(FLASH->CR, FLASH_CR_LOCK)) {
            INCONSISTENT("failed to unlock");
        }
    }
}

// flash_ob_lock()
//
// Enable write access to "option bytes".
// - also does "launch" when done
// - also locks/unlocks the main flash
//
    void
flash_ob_lock(bool lock)
{
    if(!lock) {
        // unlock sequence
        if(READ_BIT(FLASH->CR, FLASH_CR_OPTLOCK)) {
            flash_unlock();

            WRITE_REG(FLASH->OPTKEYR, FLASH_OPTKEY1);
            WRITE_REG(FLASH->OPTKEYR, FLASH_OPTKEY2);

            if(READ_BIT(FLASH->CR, FLASH_CR_OPTLOCK)) {
                INCONSISTENT("failed to OB unlock");
            }
        }
    } else {

        // write changes to OB flash bytes

        // Set OPTSTRT bit
        SET_BIT(FLASH->CR, FLASH_CR_OPTSTRT);

        /// Wait for update to complete
        _flash_wait_done();

        // lock OB again.
        SET_BIT(FLASH->CR, FLASH_CR_OPTLOCK);

        // include "launch" to make them take effect NOW
        SET_BIT(FLASH->CR, FLASH_CR_OBL_LAUNCH);

        _flash_wait_done();
    }
}


// flash_burn()
//
// My simplified version of HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, ...)
//
// NOTES:
//  - this function **AND** everything it calls, must be in RAM
//  - interrupts are already off here (entire bootloader)
//  - return non-zero on failure; don't try to handle anything
//
    __attribute__((section(".ramfunc")))
    __attribute__((noinline))
    int
flash_burn(uint32_t address, uint64_t val)
{
    uint32_t    rv;

    // just in case?
    _flash_wait_done();

    // clear any and all errors
    FLASH->SR = FLASH->SR & 0xffff;

    // disable data cache
    __HAL_FLASH_DATA_CACHE_DISABLE();

    // Program double-word (64-bit) at a specified address
    // see FLASH_Program_DoubleWord(Address, Data);

    // Set PG bit
    CLEAR_BIT(FLASH->CR, (FLASH_CR_PG | FLASH_CR_MER1 | FLASH_CR_PER | FLASH_CR_PNB));      // added
    SET_BIT(FLASH->CR, FLASH_CR_PG);

    // Program a double word
    *(__IO uint32_t *)(address) = (uint32_t)val;
    *(__IO uint32_t *)(address+4) = (uint32_t)(val >> 32);

    rv = _flash_wait_done();
    if(rv) return rv;

    // If the program operation is completed, disable the PG or FSTPG Bit
    CLEAR_BIT(FLASH->CR, FLASH_CR_PG);

    // Flush the caches to be sure of data consistency, and reenable.
    __HAL_FLASH_DATA_CACHE_RESET();
    __HAL_FLASH_DATA_CACHE_ENABLE();

    return 0;
}

#if 0
// flash_burn_fast()
//
// My simplified version of FLASH_Program_Fast()
//
// NOTES:
//  - this function **AND** everything it calls, must be in RAM
//  - interrupts are already off here (entire bootloader)
//  - return non-zero on failure; don't try to handle anything
//  - needs 128 bytes
//  - PROBLEM: Requires bank erase, and we can't do that in any case.
//
    __attribute__((section(".ramfunc")))
    __attribute__((noinline))
    int
flash_burn_fast(uint32_t address, const uint32_t values[2*FLASH_NB_DOUBLE_WORDS_IN_ROW])
{
    uint32_t    rv;

    // just in case?
    _flash_wait_done();

    // clear any and all errors
    FLASH->SR = FLASH->SR & 0xffff;

    // disable data cache
    __HAL_FLASH_DATA_CACHE_DISABLE();

    // Set FSTPG bit
    SET_BIT(FLASH->CR, FLASH_CR_FSTPG);

    for(int i=0; i<2*FLASH_NB_DOUBLE_WORDS_IN_ROW; i++, address++) {
        *(__IO uint32_t *)(address) = values[i];
    }

    rv = _flash_wait_done();
    if(rv) return rv;

    // If the program operation is completed, disable the FSTPG Bit
    CLEAR_BIT(FLASH->CR, FLASH_CR_FSTPG);

    // Flush the caches to be sure of data consistency, and reenable.
    __HAL_FLASH_DATA_CACHE_RESET();
    __HAL_FLASH_DATA_CACHE_ENABLE();

    return 0;
}
#endif

// flash_page_erase()
//
// See HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *pEraseInit, uint32_t *PageError)
//
    __attribute__((section(".ramfunc")))
    __attribute__((noinline))
    int
flash_page_erase(uint32_t address)
{
    uint32_t    page_num = (address & 0x7ffffff) / FLASH_PAGE_SIZE;

    // protect ourselves!
    if(page_num < ((BL_FLASH_SIZE + BL_NVROM_SIZE) / FLASH_PAGE_SIZE)) {
        return 1;
    }

    // always operate on both banks.
    bool bank2 = (page_num >= 256);
    page_num &= 0xff;

    // just in case?
    _flash_wait_done();

    // clear any and all errors
    FLASH->SR = FLASH->SR & 0xffff;

    // disable data cache
    __HAL_FLASH_DATA_CACHE_DISABLE();

    // choose appropriate bank to work on.
    if(bank2) {
        SET_BIT(FLASH->CR, FLASH_CR_BKER);
    } else {
        CLEAR_BIT(FLASH->CR, FLASH_CR_BKER);
    }

    // Proceed to erase the page
    MODIFY_REG(FLASH->CR, FLASH_CR_PNB, (page_num << POSITION_VAL(FLASH_CR_PNB)));
    SET_BIT(FLASH->CR, FLASH_CR_PER);
    SET_BIT(FLASH->CR, FLASH_CR_STRT);

    // Wait til done
    _flash_wait_done();

    // If the erase operation is completed, disable the PER Bit
    CLEAR_BIT(FLASH->CR, (FLASH_CR_PER | FLASH_CR_PNB));

    // Flush the caches to be sure of data consistency, and reenable.
    __HAL_FLASH_DATA_CACHE_RESET();
    __HAL_FLASH_DATA_CACHE_ENABLE();

    return 0;
}


// pick_pairing_secret()
//
    static void
pick_pairing_secret(void)
{
    // important the RNG works here. ok to call setup multiple times.
    rng_setup();

    // Demo to anyone watching that the RNG is working, but likely only
    // to be seen by production team during initial powerup.
    uint8_t    tmp[1024];
    for(int i=0; i<200; i++) {
        rng_buffer(tmp, sizeof(tmp));

        oled_show_raw(sizeof(tmp), (void *)tmp);
    }

    oled_factory_busy();

    // .. but don't use those numbers, because those are semi-public now.
    uint32_t secret[8];
    for(int i=0; i<8; i++) {
        secret[i] = rng_sample();
    }

    // enforce policy that first word is not all ones (so it never
    // looks like unprogrammed flash).
    while(secret[0] == ~0) {
        secret[0] = rng_sample();
    }

    // NOTE: if any of these 64-bit words have been programmed already once, this will
    // fail because we are not pre-erasing them. However, this area is expected
    // to be written exactly once in product's lifecycle so that should be okay.

    // Write pairing secret into flash
    {
        uint32_t dest = (uint32_t)&rom_secrets->pairing_secret;

        flash_unlock();
        for(int i=0; i<8; i+=2, dest += 8) {
            uint64_t    val = (((uint64_t)secret[i]) << 32) | secret[i+1];

            if(flash_burn(dest, val)) {
                INCONSISTENT("flash fail");
            }
        }
        flash_lock();
    }

    // Also at this point, pick some RNG noise to use as our non-changing
    // bits of various things.
    {
        uint32_t dest = (uint32_t)&rom_secrets->hash_cache_secret;
        const uint32_t blen = sizeof(rom_secrets->hash_cache_secret) 
                                + sizeof(rom_secrets->mcu_hmac_key);

        STATIC_ASSERT(offsetof(rom_secrets_t, hash_cache_secret) % 8 == 0);
        STATIC_ASSERT(blen % 8 == 0);

        flash_unlock();
        for(int i=0; i<blen; i+=8, dest += 8) {
            uint64_t    val = ((uint64_t)rng_sample() << 32) | rng_sample();

            if(flash_burn(dest, val)) {
                INCONSISTENT("flash fail");
            }
        }
        flash_lock();
    }
    
}

// confirm_pairing_secret()
//
    static void
confirm_pairing_secret(void)
{
    // Concern: if the above is interrupted (by an evil user), then we might program only
    // the first 64-bits and the rest would be ones. Easy to brute-force from there.
    // Solution: write also the XOR of the right value, and check at boot time.
    // LATER: probably not a concern because flash is ECC-checked on this chip.
    // BUT: so we are just using this to mark the 2nd half of a two-phase commit w.r.t SE1 setup

    uint64_t *src = (uint64_t *)&rom_secrets->pairing_secret;
    uint32_t dest = (uint32_t)&rom_secrets->pairing_secret_xor;

    flash_unlock();
    for(int i=0; i<(32/8); i++, dest+=8, src++) {
        uint64_t    val = ~(*src);

        if(flash_burn(dest, val)) {
            INCONSISTENT("flash xor fail");
        }
    }
    flash_lock();
}

// flash_save_ae_serial()
//
// Write the serial number of ATECC608 into flash forever.
//
    void
flash_save_ae_serial(const uint8_t serial[9])
{

    uint64_t    tmp[2];
    memset(&tmp, 0x0, sizeof(tmp));
    memcpy(&tmp, serial, 9);

    flash_setup0();
    flash_unlock();

    if(flash_burn((uint32_t)&rom_secrets->ae_serial_number[0], tmp[0])) {
        INCONSISTENT("fail1");
    }
    if(flash_burn((uint32_t)&rom_secrets->ae_serial_number[1], tmp[1])) {
        INCONSISTENT("fail2");
    }

    flash_lock();
}

// flash_save_bag_number()
//
// Write bag number (probably a string)
//
    void
flash_save_bag_number(const uint8_t new_number[32])
{

    uint32_t dest = (uint32_t)&rom_secrets->bag_number[0];
    uint64_t *src = (uint64_t *)new_number;

    flash_setup0();
    flash_unlock();

    // NOTE: can only write once! No provision for read/check, and write
    // when non-ones will fail.
    for(int i=0; i<(32/8); i++, dest+=8, src++) {
        if(flash_burn(dest, *src)) {
            INCONSISTENT("fail write");
        }
    }

    flash_lock();
}

// flash_save_se2_data()
//
// Save bunch of stuff related to SE2. Allow updates to sections that are
// given as ones at this point.
//
    void
flash_save_se2_data(const se2_secrets_t *se2)
{
    uint8_t *dest = (uint8_t *)&rom_secrets->se2;
    uint8_t *src = (uint8_t *)se2;

    STATIC_ASSERT(offsetof(rom_secrets_t, se2) % 8 == 0);

    flash_setup0();
    flash_unlock();

    for(int i=0; i<(sizeof(se2_secrets_t)/8); i++, dest+=8, src+=8) {
        uint64_t val;
        memcpy(&val, src, sizeof(val));

        // don't write if all ones or already written correctly
        if(val == ~0) continue;
        if(check_equal(dest, src, 8)) continue;

        // can't write if not ones already
        ASSERT(check_all_ones(dest, 8));

        if(flash_burn((uint32_t)dest, val)) {
            INCONSISTENT("fail write");
        }
    }

    flash_lock();
}

// flash_setup()
//
// This is really a state-machine, to recover boards that are booted w/ missing AE chip.
//
    void
flash_setup(void)
{
    flash_setup0();

    STATIC_ASSERT(sizeof(rom_secrets_t) <= 0x2000);

    // see if we have picked a pairing secret yet.
    // NOTE: critical section for glitching (at least in past versions)
    //  - check_all.. functions have a rng_delay in them already
    rng_delay();
    bool blank_ps = check_all_ones(rom_secrets->pairing_secret, 32);
    bool zeroed_ps = check_all_zeros(rom_secrets->pairing_secret, 32);
    bool blank_xor = check_all_ones(rom_secrets->pairing_secret_xor, 32);
    bool blank_ae = (~rom_secrets->ae_serial_number[0] == 0);
    rng_delay();

    if(zeroed_ps) {
        // fast brick process leaves us w/ zero pairing secret
        oled_show(screen_brick);
        LOCKUP_FOREVER();
    }

    if(blank_ps) {
        // get some good entropy, save it.
        pick_pairing_secret();

        blank_ps = false;
    }

    if(blank_xor || blank_ae) {

        // setup the SE2 (mostly). handles failures by dying
        se2_setup_config();

        // configure and lock-down the SE1
        int rv = ae_setup_config();

        rng_delay();
        if(rv) {
            // Hardware fail speaking to AE chip ... be careful not to brick here.
            // Do not continue!! We might fix the board, or add missing pullup, etc.
            oled_show(screen_se1_issue);
            puts("SE1 config fail");

            LOCKUP_FOREVER();
        }

        rng_delay();
        if(blank_xor) {
            // write secret again, complemented, to indicate successful AE programming
            confirm_pairing_secret();
        }

        // real power cycle required now.
        oled_show(screen_replug);
        puts("replug required");

        LOCKUP_FOREVER();
    }

    rng_delay();
    if(!blank_ps && !blank_xor) {
        // check the XOR value also written: 2 phase commit
        uint8_t tmp[32];
        memcpy(tmp, rom_secrets->pairing_secret, 32);
        xor_mixin(tmp, rom_secrets->pairing_secret_xor, 32);

        if(!check_all_ones(tmp, 32)) {
            oled_show(screen_corrupt);
            puts("corrupt pair sec");

            // dfu won't save them here, so just die
            LOCKUP_FOREVER();
        }
    }

    // TODO: maybe check option bytes and protections
    // implied by that are in place. If wrong, do the
    // appropriate lockdown, which might be one-way.
    // That's fine if we intend to ship units locked already.
    
    // Do NOT do write every boot, as it might wear-out
    // the flash bits in OB.

}

// flash_lockdown_hard()
//
// Configure the OB (option bytes) to values that:
// - ensure bootloader isn't overwritten easily.
// - enable level 2 flash protect
// - once level 2 is set, no going back.
// 
// This is a one-way trip. Might need power cycle to (fully?) take effect.
//
    void
flash_lockdown_hard(uint8_t rdp_level_code)
{
    flash_setup0();

    // see FLASH_OB_WRPConfig()

    flash_ob_lock(false);
        // lock first 32k against any writes
        FLASH->WRP1AR = (num_pages_locked << 16);
        FLASH->WRP1BR = 0xff;      // unused.
        FLASH->WRP2AR = 0xff;      // unused.
        FLASH->WRP2BR = 0xff;      // unused.

        // PCRO = Proprietary Code Read-Out (protection)
        // - isn't useful to us (doesn't protect data, exec-only code)
        // - "In case the Level 1 is configured and no PCROP area is defined,
        //    it is mandatory to set PCROP_RDP bit to 1 (full mass erase when
        //    the RDP level is decreased from Level 1 to Level 0)."
        // - D-bus access blocked, even for code running inside the PCROP area! (AN4758)
        //   So literal values and constant tables and such would need special linking.

        // set protection level
        uint32_t was = FLASH->OPTR & ~0xff;
        FLASH->OPTR = was | rdp_level_code;    // select level X, other values as observed

    flash_ob_lock(true);
}

// record_highwater_version()
//
    int
record_highwater_version(const uint8_t timestamp[8])
{
    const uint8_t *otp = (const uint8_t *)OPT_FLASH_BASE;

    ASSERT(timestamp[0] < 0x40);
    ASSERT(timestamp[0] >= 0x10);

    // just write to first blank slot we can find.
    for(int i=0; i<NUM_OPT_SLOTS; i++, otp+=8) {
        if(check_all_ones(otp, 8)) {
            // here.
            uint64_t val = 0;
            memcpy(&val, timestamp, 8);

            flash_setup0();

            flash_unlock();
                flash_burn((uint32_t)otp, val);
            flash_lock();

            return 0;
        }
    }

    // no space.
    return 1;
}

// mcu_key_get()
//
    const mcu_key_t *
mcu_key_get(bool *valid)
{
    // get current "mcu_key" value; first byte will never be 0x0 or 0xff
    // - except if no key set yet/recently wiped
    // - if none set, returns ptr to first available slot which will be all ones
    const mcu_key_t *ptr = rom_secrets->mcu_keys, *avail=NULL;

    for(int i=0; i<numberof(rom_secrets->mcu_keys); i++, ptr++) {
        if(ptr->value[0] == 0xff) {
            if(!avail) {
                avail = ptr;
            }
        } else if(ptr->value[0] != 0x00) {
            rng_delay();
            *valid = true;
            return ptr;
        }
    }

    rng_delay();
    *valid = false;
    return avail;
}

// mcu_key_clear()
//
    void
mcu_key_clear(const mcu_key_t *cur)
{
    if(!cur) {
        bool valid;
        cur = mcu_key_get(&valid);

        if(!valid) return;
    }

    STATIC_ASSERT(offsetof(rom_secrets_t, mcu_keys) % 8 == 0);

    // no delays here since decision has been made, and don't 
    // want to give them more time to interrupt us
    flash_setup0();
    flash_unlock();
        uint32_t  pos = (uint32_t)cur;
        flash_burn(pos, 0); pos += 8;
        flash_burn(pos, 0); pos += 8;
        flash_burn(pos, 0); pos += 8;
        flash_burn(pos, 0);
    flash_lock();
}

// mcu_key_pick()
//
    const mcu_key_t *
mcu_key_pick(void)
{
    mcu_key_t       n;

    // get some good entropy, and whiten it just in case.
    do { 
        rng_buffer(n.value, 32);
        sha256_single(n.value, 32, n.value);
        sha256_single(n.value, 32, n.value);
    } while(n.value[0] == 0x0 || n.value[0] == 0xff);

    const mcu_key_t *cur;

    do {
        bool valid = false; 
        cur = mcu_key_get(&valid);

        if(!cur) {
            // no free slots. we are brick.
            puts("mk full");
            oled_show(screen_brick);

            LOCKUP_FOREVER();
        }

        if(valid) {
            // clear existing key, if it's defined.
            ASSERT(cur->value[0] != 0x00);
            ASSERT(cur->value[0] != 0xff);

            mcu_key_clear(cur);

            continue;
        }
    } while(0);
    
    // burn it
    flash_setup0();
    flash_unlock();
        uint32_t  pos = (uint32_t)cur;
        const uint8_t   *fr = n.value;

        for(int i=0; i<32; i+= 8, pos += 8, fr += 8) {
            uint64_t v;
            memcpy(&v, fr, sizeof(v));

            flash_burn(pos, v);
        }
    flash_lock();

#if 1
    // check it
    bool valid = false; 
    const mcu_key_t *after = mcu_key_get(&valid);
    ASSERT(valid);
    ASSERT(after == cur);
#endif

    return cur;
}

// mcu_fast_brick()
//
    void
mcu_fast_brick(void)
{
    flash_setup0();
    flash_unlock();
        // simply erase all the critical secrets
        flash_page_erase(BL_NVROM_BASE);

        // but then write zeros so it doesn't look like unprogrammed part
        for(uint32_t  pos = BL_NVROM_BASE, i=0; i<64/8; i++, pos += 8) {
            flash_burn(pos, 0);
        }
    flash_lock();
    
    puts("fast brck");
    oled_show(screen_brick);

    LOCKUP_FOREVER();
}

// EOF
