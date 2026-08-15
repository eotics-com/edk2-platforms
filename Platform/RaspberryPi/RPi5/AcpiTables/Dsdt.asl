/** @file
 *
 *  Differentiated System Definition Table (DSDT)
 *
 *  Copyright (c) 2023-2024, Mario Bălănică <mariobalanica02@gmail.com>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include <IndustryStandard/Bcm2712.h>
#include <RpiPlatformVarStoreData.h>

#include "AcpiTables.h"

DefinitionBlock ("Dsdt.aml", "DSDT", 2, "RPIFDN", "RPI5    ", 2)
{
  Scope (\_SB_)
  {
    Device (CPU0) {
      Name (_HID, "ACPI0007")
      Name (_UID, 0x0)
      Name (_STA, 0xf)
    }

    Device (CPU1) {
      Name (_HID, "ACPI0007")
      Name (_UID, 0x1)
      Name (_STA, 0xf)
    }

    Device (CPU2) {
      Name (_HID, "ACPI0007")
      Name (_UID, 0x2)
      Name (_STA, 0xf)
    }

    Device (CPU3) {
      Name (_HID, "ACPI0007")
      Name (_UID, 0x3)
      Name (_STA, 0xf)
    }

    //
    // VideoCore property-mailbox telemetry. The platform driver patches these
    // addresses and capability masks only after probing the running firmware.
    //
    Name (MBEN, ACPI_PATCH_BYTE_VALUE)
    Name (PMEN, ACPI_PATCH_BYTE_VALUE) // Standard ACPI power meter available
    Name (MBPA, ACPI_PATCH_QWORD_VALUE) // Mailbox MMIO address
    Name (MBCA, ACPI_PATCH_QWORD_VALUE) // CPU address of shared command page
    Name (MBBA, ACPI_PATCH_QWORD_VALUE) // VideoCore bus address of command page
    Name (TMLO, ACPI_PATCH_DWORD_VALUE) // Valid firmware temperature IDs
    Name (VMLO, ACPI_PATCH_DWORD_VALUE) // Valid firmware voltage IDs
    Name (RTCP, ACPI_PATCH_DWORD_VALUE) // Valid RTC telemetry registers
    Name (PVLO, ACPI_PATCH_DWORD_VALUE) // Valid PMIC voltage ADC IDs
    Name (PILO, ACPI_PATCH_DWORD_VALUE) // Valid PMIC current ADC IDs
    Name (PSMW, ACPI_PATCH_DWORD_VALUE) // Power-source maximum output, mW
    Name (PSMC, ACPI_PATCH_DWORD_VALUE) // Advertised source current, mA
    Name (PSRR, ACPI_PATCH_DWORD_VALUE) // PMIC reset reason
    Name (PSUH, ACPI_PATCH_DWORD_VALUE) // USB high-current mode
    Name (PSOC, ACPI_PATCH_DWORD_VALUE) // USB over-current detected

    OperationRegion (MBXR, SystemMemory, MBPA, 0x24)
    Field (MBXR, DWordAcc, NoLock, Preserve) {
      MBRD, 32,
      Offset (0x18),
      MBST, 32,
      Offset (0x20),
      MBWR, 32
    }

    OperationRegion (MBCM, SystemMemory, MBCA, 0x1000)
    Field (MBCM, DWordAcc, NoLock, Preserve) {
      MBSZ, 32,
      MBRS, 32,
      MBTG, 32,
      MBTS, 32,
      MBVS, 32,
      MBID, 32,
      MBVL, 32,
      MBET, 32,
      Offset (0xF00),
      FWAC, 32,
      Offset (0xF40),
      ACAC, 32,
      Offset (0xF80),
      MBTR, 32
    }

    // GET_GENCMD_RESULT overlays the generic tag value at offset 0x14. The
    // first DWORD is command status and the remaining 2044 bytes contain the
    // command or response string. Its end tag is at offset 0x814.
    Field (MBCM, ByteAcc, NoLock, Preserve) {
      Offset (0x14),
      GCST, 32,
      Offset (0x18),
      GCDB, 16352,
      Offset (0x814),
      GCET, 32
    }

    Method (MBLK, 0, Serialized) {
      ACAC = One
      MBTR = Zero // Give priority to the firmware runtime-service side.

      If ((FWAC != Zero) && (MBTR == Zero)) {
        ACAC = Zero
        Return (Zero)
      }

      Return (One)
    }

    Method (MBUL, 0, Serialized) {
      ACAC = Zero
    }

    Method (MBIO, 0, Serialized) {
      // Drain stale responses before issuing a new property request.
      Local0 = Zero
      While ((MBST & 0x40000000) == Zero) {
        Local1 = MBRD
        Local0++
        If (Local0 >= 64) {
          Return (Zero)
        }
      }

      // Wait at most 10 ms for room in the outbound mailbox.
      Local0 = Zero
      While (MBST & 0x80000000) {
        Stall (10)
        Local0++
        If (Local0 >= 1000) {
          Return (Zero)
        }
      }

      // Read both possible end-tag locations before ringing the doorbell.
      Local1 = MBET
      Local1 = GCET
      Local2 = MBBA | 8
      MBWR = Local2

      // Wait at most 10 ms and consume unrelated channel responses.
      Local0 = Zero
      While (Local0 < 1000) {
        If ((MBST & 0x40000000) == Zero) {
          Local1 = MBRD
          If (Local1 == Local2) {
            Return (One)
          }
        }

        Stall (10)
        Local0++
      }

      Return (Zero)
    }

    Method (MBTX, 2, Serialized) {
      Local3 = Ones
      If ((MBEN != One) || (MBLK () == Zero)) {
        Return (Local3)
      }

      MBSZ = 32
      MBRS = Zero
      MBTG = Arg0
      MBTS = 8
      MBVS = Zero
      MBID = Arg1
      MBVL = Zero
      MBET = Zero

      If (MBIO () &&
          (MBRS == 0x80000000) &&
          (MBVS & 0x80000000) &&
          ((MBVS & 0x7FFFFFFF) >= 8))
      {
        Local3 = MBVL
      }

      MBUL ()
      Return (Local3)
    }

    Method (GCMQ, 1, Serialized) {
      Local7 = Buffer () {}
      If ((MBEN != One) || (MBLK () == Zero)) {
        Return (Local7)
      }

      GCDB = Buffer (0x7FC) {}
      GCDB = ToBuffer (Arg0)
      MBSZ = 0x818
      MBRS = Zero
      MBTG = 0x00030080
      MBTS = 0x800
      MBVS = Zero
      GCST = Zero
      GCET = Zero

      If (MBIO () &&
          (MBRS == 0x80000000) &&
          (MBVS & 0x80000000) &&
          ((MBVS & 0x7FFFFFFF) >= 4) &&
          (GCST == Zero))
      {
        Local7 = GCDB
      }

      MBUL ()
      Return (Local7)
    }

    // Parse pmic_read_adc's channel-id and decimal value, returning microvolts
    // or microamps. The channel IDs are unique across both measurement types.
    Method (PVAL, 2, Serialized) {
      Local0 = SizeOf (Arg0)
      Local1 = Zero
      While (Local1 < Local0) {
        Local2 = DerefOf (Index (Arg0, Local1))
        If (Local2 == Zero) {
          Return (Ones)
        }

        If (Local2 == 0x28) { // '('
          Local3 = Zero
          Local4 = Zero
          Local1++
          While (Local1 < Local0) {
            Local2 = DerefOf (Index (Arg0, Local1))
            If ((Local2 < 0x30) || (Local2 > 0x39)) {
              Break
            }

            Local3 = (Local3 * 10) + Local2 - 0x30
            Local4 = One
            Local1++
          }

          If (Local4 && (Local2 == 0x29) && (Local3 == Arg1)) { // ')'
            While (Local1 < Local0) {
              Local2 = DerefOf (Index (Arg0, Local1))
              If ((Local2 == Zero) || (Local2 == 0x0A)) {
                Return (Ones)
              }

              If (Local2 == 0x3D) { // '='
                Break
              }

              Local1++
            }

            Local1++
            Local3 = Zero // Integer part
            Local4 = Zero // At least one digit
            While (Local1 < Local0) {
              Local2 = DerefOf (Index (Arg0, Local1))
              If ((Local2 < 0x30) || (Local2 > 0x39)) {
                Break
              }

              Local3 = (Local3 * 10) + Local2 - 0x30
              Local4 = One
              Local1++
            }

            If (Local4 == Zero) {
              Return (Ones)
            }

            Local5 = Zero // Fractional digits consumed
            Local6 = Zero // Fraction scaled to millionths
            If (Local2 == 0x2E) { // '.'
              Local1++
              While (Local1 < Local0) {
                Local2 = DerefOf (Index (Arg0, Local1))
                If ((Local2 < 0x30) || (Local2 > 0x39)) {
                  Break
                }

                If (Local5 < 6) {
                  Local6 = (Local6 * 10) + Local2 - 0x30
                }

                Local5++
                Local1++
              }
            }

            While (Local5 < 6) {
              Local6 *= 10
              Local5++
            }

            If ((Local2 == 0x41) || (Local2 == 0x56)) { // 'A' or 'V'
              Return ((Local3 * 1000000) + Local6)
            }

            Return (Ones)
          }
        }

        Local1++
      }

      Return (Ones)
    }

    Method (GTMP, 1, Serialized) {
      If ((Arg0 > 31) || ((TMLO & (One << Arg0)) == Zero)) {
        Return (Ones)
      }

      Return (MBTX (0x00030006, Arg0))
    }

    Method (GVLT, 1, Serialized) {
      If ((Arg0 > 31) || ((VMLO & (One << Arg0)) == Zero)) {
        Return (Ones)
      }

      Return (MBTX (0x00030003, Arg0))
    }

    Method (GRTC, 1, Serialized) {
      If ((Arg0 > 31) || ((RTCP & (One << Arg0)) == Zero)) {
        Return (Ones)
      }

      Return (MBTX (0x00030087, Arg0))
    }

    Method (GPAD, 1, Serialized) {
      If ((Arg0 > 31) || (((PVLO | PILO) & (One << Arg0)) == Zero)) {
        Return (Ones)
      }

      Local0 = GCMQ ("pmic_read_adc")
      Return (PVAL (Local0, Arg0))
    }

    Method (RPWV, 3, Serialized) {
      Local0 = PVAL (Arg0, Arg1)
      Local1 = PVAL (Arg0, Arg2)
      If ((Local0 == Ones) || (Local1 == Ones)) {
        Return (Ones)
      }

      // microamps * microvolts / 1,000,000,000 = milliwatts.
      Return ((Local0 * Local1) / 1000000000)
    }

    Method (RPWR, 1, Serialized) {
      Local0 = GCMQ ("pmic_read_adc")
      Switch (ToInteger (Arg0)) {
        Case (0)  { Return (RPWV (Local0, 0, 8)) }
        Case (1)  { Return (RPWV (Local0, 1, 9)) }
        Case (2)  { Return (RPWV (Local0, 2, 10)) }
        Case (3)  { Return (RPWV (Local0, 3, 11)) }
        Case (4)  { Return (RPWV (Local0, 4, 12)) }
        Case (5)  { Return (RPWV (Local0, 5, 13)) }
        Case (6)  { Return (RPWV (Local0, 6, 14)) }
        Case (7)  { Return (RPWV (Local0, 7, 15)) }
        Case (16) { Return (RPWV (Local0, 16, 19)) }
        Case (17) { Return (RPWV (Local0, 17, 20)) }
        Case (18) { Return (RPWV (Local0, 18, 21)) }
        Case (22) { Return (RPWV (Local0, 22, 23)) }
      }

      Return (Ones)
    }

    Method (TPWR, 0, Serialized) {
      Local0 = GCMQ ("pmic_read_adc")
      Local1 = Zero
      Local2 = Zero

      Local3 = RPWV (Local0, 0, 8)
      If (Local3 != Ones) { Local1 += Local3; Local2++ }
      Local3 = RPWV (Local0, 1, 9)
      If (Local3 != Ones) { Local1 += Local3; Local2++ }
      Local3 = RPWV (Local0, 2, 10)
      If (Local3 != Ones) { Local1 += Local3; Local2++ }
      Local3 = RPWV (Local0, 3, 11)
      If (Local3 != Ones) { Local1 += Local3; Local2++ }
      Local3 = RPWV (Local0, 4, 12)
      If (Local3 != Ones) { Local1 += Local3; Local2++ }
      Local3 = RPWV (Local0, 5, 13)
      If (Local3 != Ones) { Local1 += Local3; Local2++ }
      Local3 = RPWV (Local0, 6, 14)
      If (Local3 != Ones) { Local1 += Local3; Local2++ }
      Local3 = RPWV (Local0, 7, 15)
      If (Local3 != Ones) { Local1 += Local3; Local2++ }
      Local3 = RPWV (Local0, 16, 19)
      If (Local3 != Ones) { Local1 += Local3; Local2++ }
      Local3 = RPWV (Local0, 17, 20)
      If (Local3 != Ones) { Local1 += Local3; Local2++ }
      Local3 = RPWV (Local0, 18, 21)
      If (Local3 != Ones) { Local1 += Local3; Local2++ }
      Local3 = RPWV (Local0, 22, 23)
      If (Local3 != Ones) { Local1 += Local3; Local2++ }

      If (Local2 == Zero) {
        Return (Ones)
      }

      Return (Local1)
    }

    // The board has no system battery. This describes its external 5 V source
    // without misrepresenting the RTC backup cell as a control-method battery.
    Device (PSRC) {
      Name (_HID, "ACPI0003")
      Name (_UID, Zero)
      Name (_STR, Unicode ("Raspberry Pi 5 external power source"))
      Name (_PCL, Package () { \_SB })

      Method (_PSR, 0, NotSerialized) {
        Return (One)
      }

      Method (_PIF, 0, NotSerialized) {
        Return (Package () {
          Zero,
          PSMW,
          0xFFFFFFFF,
          "USB-C, PoE, or fixed 5 V input",
          "",
          "Maximum power is the boot-firmware advertised 5 V capability"
        })
      }
    }

    // Standard ACPI power meter for the sum of PMIC-managed output rails.
    // It excludes direct 5 V loads and conversion losses. _PMD is deliberately
    // omitted because the rail sum cannot be mapped accurately to complete
    // ACPI device objects and therefore must not claim whole-system input power.
    Device (PMTR) {
      Name (_HID, "ACPI000D")
      Name (_UID, Zero)
      Name (_STR, Unicode ("DA9091 PMIC managed-rail power"))

      Method (_STA, 0, NotSerialized) {
        Return (PMEN * 0xF)
      }

      Method (_PMC, 0, NotSerialized) {
        Return (Package () {
          One,             // Measurement supported
          Zero,            // Milliwatts
          One,             // Output power
          Zero,            // No published aggregate accuracy guarantee
          Zero,            // On-demand sample; no firmware-side cache
          Zero,            // No configurable averaging interval
          Zero,
          0xFFFFFFFF,      // Hysteresis unavailable
          Zero,            // No configurable hardware limit
          Zero,
          Zero,
          "DA9091",
          "",
          "Sum of PMIC-managed DC rail outputs"
        })
      }

      Method (_PMM, 0, Serialized) {
        Return (TPWR ())
      }
    }

    Device (FTEL) {
      Name (_HID, "RPI0005")
      Name (_UID, Zero)
      Name (_STR, Unicode ("Raspberry Pi firmware telemetry"))

      Method (_STA, 0, NotSerialized) {
        If (MBEN == One) {
          Return (0xF)
        }

        Return (Zero)
      }

      // 31fdd5d5-6d36-47b7-bce8-74690d661c0c
      // Function 1 returns { revision, temperature mask, legacy voltage mask,
      // RTC-register mask, PMIC voltage mask, PMIC current mask }.
      // Functions 2-7 take one ID in Arg3 and return mC, uV, raw RTC data, uV,
      // uA, or mW respectively. Function 8 returns boot power-source status.
      Method (_DSM, 4, Serialized) {
        If ((Arg0 == ToUUID ("31fdd5d5-6d36-47b7-bce8-74690d661c0c")) &&
            (Arg1 == One))
        {
          Switch (ToInteger (Arg2)) {
            Case (Zero) {
              Return (Buffer () { 0xFF, 0x01 })
            }

            Case (One) {
              Return (Package () { One, TMLO, VMLO, RTCP, PVLO, PILO })
            }

            Case (2) {
              If (SizeOf (Arg3) >= One) {
                Return (GTMP (DerefOf (Index (Arg3, Zero))))
              }
            }

            Case (3) {
              If (SizeOf (Arg3) >= One) {
                Return (GVLT (DerefOf (Index (Arg3, Zero))))
              }
            }

            Case (4) {
              If (SizeOf (Arg3) >= One) {
                Return (GRTC (DerefOf (Index (Arg3, Zero))))
              }
            }

            Case (5) {
              If (SizeOf (Arg3) >= One) {
                Local0 = DerefOf (Index (Arg3, Zero))
                If ((Local0 <= 31) && (PVLO & (One << Local0))) {
                  Return (GPAD (Local0))
                }
              }
            }

            Case (6) {
              If (SizeOf (Arg3) >= One) {
                Local0 = DerefOf (Index (Arg3, Zero))
                If ((Local0 <= 31) && (PILO & (One << Local0))) {
                  Return (GPAD (Local0))
                }
              }
            }

            Case (7) {
              If (SizeOf (Arg3) >= One) {
                Return (RPWR (DerefOf (Index (Arg3, Zero))))
              }
            }

            Case (8) {
              Return (Package () { PSMC, PSMW, PSRR, PSUH, PSOC })
            }
          }
        }

        Return (Buffer () { 0x00 })
      }
    }

    //
    // Legacy SOC bus
    //
    Device (SOCB) {
      Name (_HID, "ACPI0004")
      Name (_UID, 0x0)
      Name (_CCA, 0x0)

      Method (_CRS, 0, Serialized) {
        //
        // Container devices with _DMA must have _CRS.
        // TO-DO: Is describing the entire MMIO range in a single resource
        // enough, or do we need to list each individual resource consumed
        // by the child devices?
        //
        Name (RBUF, ResourceTemplate () {
          QWORDMEMORY_BUF (00, ResourceProducer)
        })
        QWORD_SET (00, BCM2712_LEGACY_BUS_BASE, BCM2712_LEGACY_BUS_LENGTH, 0)
        Return (RBUF)
      }

      Name (_DMA, ResourceTemplate () {
        //
        // Only the first GB is available.
        // Bus 0xC0000000 -> CPU 0x00000000.
        //
        QWordMemory (ResourceProducer,
          PosDecode,
          MinFixed,
          MaxFixed,
          NonCacheable,
          ReadWrite,
          0x0,
          0x00000000C0000000, // MIN
          0x00000000FFFFFFFF, // MAX
          0xFFFFFFFF40000000, // TRA
          0x0000000040000000, // LEN
          ,
          ,
          )
      })

      //
      // PL011 Debug UART Port
      //
      Device (URT0) {
        Name (_HID, "ARMH0011")
        Name (_UID, 0x0)
        Name (_CCA, 0x0)

        Method (_CRS, 0x0, Serialized) {
          Name (RBUF, ResourceTemplate () {
            QWORDMEMORY_BUF (00, ResourceConsumer)
            Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { PL011_DEBUG_INTERRUPT }
          })
          QWORD_SET (00, PL011_DEBUG_BASE_ADDRESS, PL011_DEBUG_LENGTH, 0)
          Return (RBUF)
        }

        Name (_DSD, Package () {
          ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
          Package () {
            Package () { "clock-frequency", PL011_DEBUG_CLOCK_FREQUENCY }
          }
        })
      }
    } // Device (SOCB)

    //
    // PCIe Root Complexes
    //
    // These (and _STA) are patched by the platform driver:
    //
    Name (PBMA, ACPI_PATCH_BYTE_VALUE)
    Name (BB32, ACPI_PATCH_QWORD_VALUE)
    Name (MS32, ACPI_PATCH_QWORD_VALUE)

    Device (PCI0) {
      Name (_SEG, 0)
      Name (_STA, 0xF)

      Name (CFGB, BCM2712_BRCMSTB_PCIE0_BASE)
      Name (CFGS, BCM2712_BRCMSTB_PCIE_LENGTH)
      Name (MB32, BCM2712_BRCMSTB_PCIE0_CPU_MEM_BASE)
      Name (MB64, BCM2712_BRCMSTB_PCIE0_CPU_MEM64_BASE)
      Name (MS64, BCM2712_BRCMSTB_PCIE_MEM64_SIZE)

      Name (_PRT, Package () {
        Package (4) { 0x0FFFF, 0, 0, 241 },
        Package (4) { 0x0FFFF, 1, 0, 242 },
        Package (4) { 0x0FFFF, 2, 0, 243 },
        Package (4) { 0x0FFFF, 3, 0, 244 }
      })

      Include ("PcieCommon.asi")
    }

    Device (PCI1) {
      Name (_SEG, 1)
      Name (_STA, 0xF)

      Name (CFGB, BCM2712_BRCMSTB_PCIE1_BASE)
      Name (CFGS, BCM2712_BRCMSTB_PCIE_LENGTH)
      Name (MB32, BCM2712_BRCMSTB_PCIE1_CPU_MEM_BASE)
      Name (MB64, BCM2712_BRCMSTB_PCIE1_CPU_MEM64_BASE)
      Name (MS64, BCM2712_BRCMSTB_PCIE_MEM64_SIZE)

      Name (_PRT, Package () {
        Package (4) { 0x0FFFF, 0, 0, 251 },
        Package (4) { 0x0FFFF, 1, 0, 252 },
        Package (4) { 0x0FFFF, 2, 0, 253 },
        Package (4) { 0x0FFFF, 3, 0, 254 }
      })

      Include ("PcieCommon.asi")
    }

    Device (PCI2) {
      Name (_SEG, 2)
      Name (_STA, 0xF)

      Name (CFGB, BCM2712_BRCMSTB_PCIE2_BASE)
      Name (CFGS, BCM2712_BRCMSTB_PCIE_LENGTH)
      Name (MB32, BCM2712_BRCMSTB_PCIE2_CPU_MEM_BASE)
      Name (MB64, BCM2712_BRCMSTB_PCIE2_CPU_MEM64_BASE)
      Name (MS64, BCM2712_BRCMSTB_PCIE_MEM64_SIZE)

      Name (_PRT, Package () {
        Package (4) { 0x0FFFF, 0, 0, 261 },
        Package (4) { 0x0FFFF, 1, 0, 262 },
        Package (4) { 0x0FFFF, 2, 0, 263 },
        Package (4) { 0x0FFFF, 3, 0, 264 }
      })

      Include ("PcieCommon.asi")
    }

    //
    // RP1 I/O Bridge
    //
    Device (RP1B) {
      Name (_HID, "ACPI0004")
      Name (_UID, 0x1)

      // Parent bus is non-coherent
      Name (_CCA, 0x0)

      // Firmware mapped BAR - patched by platform driver
      Name (PBAR, ACPI_PATCH_QWORD_VALUE)

      // Shared level interrupt - PCIE2 INTA# SPI
      Name (PINT, 261)

      Method (_STA) {
        If (PBAR == ACPI_PATCH_QWORD_VALUE) {
          Return (0x0)
        }
        Return (0xF)
      }

      Include ("Rp1.asi")
    }

    //
    // Broadcom STB SDHCI controllers (Arasan IP)
    //
    // There are 2 notable quirks with these controllers:
    // 1) Broken 1.8v signaling switch: instead it's changed via an external
    //    regulator. Thankfully, Intel Bay Trail had the same issue, so we
    //    can pretend to be one of their affected HCs and reuse the _DSM
    //    workaround.
    //
    // 2) Capability claims hardware retuning is supported, but it causes issues.
    //    Windows will crash when switching to SDR50/SDR104. Linux does not appear
    //    to care, but we still override the "sdhci-caps-mask" property just in case.
    //
    // Supposedly there's a 32-bit bus access limitation too (inherited from BCM283x),
    // but no issues have actually been observed under stress test in both Windows
    // and Linux. Chances are this was fixed in the production BCM2712C0 stepping.
    //
    // We provide two compatibility modes:
    // 1) BRCMSTB _HID + Bay Trail _CID:
    //    - Windows binds to "VEN_8086&DEV_0F14" and has DDR50 with _DSM working.
    //      SDR104/50 modes can be enabled by a sdbus driver override.
    //
    //    - Linux recognizes "80860F16" but treats the controller as plain SDHCI and
    //      no _DSM, we limit the speed to HS via "sdhci-caps-mask".
    //
    //    - FreeBSD binds to "80860F16" but does not implement the _DSM nor the _DSD
    //      for caps override, fortunately it just falls back to HS.
    //
    // 2) Full Bay Trail _HID: this enables Linux to see the device as proper Bay Trail
    //    and use the _DSM. DDR50 is also enabled by relaxing the caps mask.
    //
    // The "Limit UHS-I" option is enabled by default in case OSes are not aware of
    // the broken retuning (i.e. Windows does not parse _DSD). It disables SDR104/50
    // since these modes depend on tuning.
    //
    // These will be patched in by the platform driver.
    //
    Name (SDCM, 0x0) // Compatibility Mode
    Name (SDLU, 0x0) // Limit UHS-I

    Device (SDC0) {
      Method (_HID) {
        If (SDCM == ACPI_SD_COMPAT_MODE_FULL_BAYTRAIL) {
          Return ("80860F16")
        } Else {
          Return ("BRCM5D12")
        }
      }
      Name (_CID, Package () { "80860F16", "VEN_8086&DEV_0F14" })
      Name (_UID, 0x0)
      Name (_CCA, 0x0)

      Method (_CRS, 0x0, Serialized) {
        Name (RBUF, ResourceTemplate () {
          QWORDMEMORY_BUF (00, ResourceConsumer)
          Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 305 }
        })
        QWORD_SET (00, BCM2712_BRCMSTB_SDIO1_HOST_BASE, BCM2712_BRCMSTB_SDIO_HOST_LENGTH, 0)
        Return (RBUF)
      }

      OperationRegion (GPIO, SystemMemory, BCM2712_BRCMSTB_GIO_AON_BASE, BCM2712_BRCMSTB_GIO_AON_LENGTH)
      Field (GPIO, DWordAcc, NoLock, Preserve) {
        Offset (0x4),
        DATA, 32,     // GPIO 3: 1.8 V select; GPIO 4: slot power
      }

      Method (_INI, 0, Serialized) {
        DATA &= ~(1 << 3)
      }

      // microSD slot power, GIO_AON pin 4.
      PowerResource (SDVD, 0x0, 0x0) {
        Method (_STA) { Return ((DATA >> 4) & 0x1) }
        Method (_ON)  { DATA |= (1 << 4); Sleep (20) }
        Method (_OFF) { DATA &= ~(1 << 4) }
      }
      Name (_PR0, Package () { SDVD })

      Method (_DSM, 4, Serialized) {
        // Check the UUID
        If (Arg0 == ToUUID ("f6c13ea5-65cd-461f-ab7a-29f7e8d5bd61")) {
          // Check the revision
          If (Arg1 >= 0) {
            // Check the function index
            Switch (ToInteger (Arg2)) {
              //
              // Supported functions:
              // Bit 0 - Indicates support for functions other than 0
              // Bit 3 - Indicates support to set 1.8V signalling
              // Bit 4 - Indicates support to set 3.3V signalling
              // Bit 8 - Indicates support for UHS-I modes
              //
              Case (0) {
                Return (Buffer () { 0x19, 0x01 }) // 0x119
              }

              // Function Index 3: Set 1.8v signalling
              Case (3) {
                DATA |= (1 << 3)
                Return (Buffer () { 0x00 })
              }

              // Function Index 4: Set 3.3v signalling
              Case (4) {
                DATA &= ~(1 << 3)
                Return (Buffer () { 0x00 })
              }

              //
              // Function Index 8: Supported UHS-I modes
              // Bit 0 - SDR25
              // Bit 1 - DDR50
              // Bit 2 - SDR50
              // Bit 3 - SDR104
              //
              Case (8) {
                // Limit UHS-I modes?
                If (SDLU == 1) {
                  Return (Buffer () { 0x02 }) // DDR50
                } Else {
                  Return (Buffer () { 0x0F }) // All
                }
              }
            } // Function index check
          } // Revision check
        } // UUID check
        Return (Buffer () { 0x0 })
      } // _DSM

      Method (_DSD, 0, Serialized) {
        // Capabilities mask
        Name (CAPM, 0x0000000000000000)

        // Start by disabling hardware retuning
        CAPM |= (1 << 47) | (1 << 46)

        // Limit UHS-I modes?
        If (SDLU == 1) {
          // Disable SDR104, SDR50
          CAPM |= (1 << 33) | (1 << 32)

          If (SDCM != ACPI_SD_COMPAT_MODE_FULL_BAYTRAIL) {
            // Additionally disable DDR50, Linux can't use
            // the _DSM for changing voltage in this case.
            CAPM |= (1 << 34)
          }
        }

        Return (Package () {
          ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
          Package () {
            Package () { "sdhci-caps-mask", CAPM }
          }
        })
      } // _DSD

      //
      // Removable SD card
      //
      Device (SDMM) {
        Name (_ADR, 0x0)

        Method (_RMV) {
          Return (1)
        }
      }
    } // Device (SDC0)

    //
    // CYW43455 WL_ON, GIO pin 28.
    //
    OperationRegion (GIOW, SystemMemory, BCM2712_BRCMSTB_GIO_BASE, BCM2712_BRCMSTB_GIO_LENGTH)
    Field (GIOW, DWordAcc, NoLock, Preserve) {
      Offset (0x4), GDAT, 32,
      Offset (0x8), GDIR, 32
    }
    PowerResource (WLPW, 0, 0) {
      Method (_STA) { Return ((GDAT >> 28) & 1) }
      Method (_ON)  { GDAT |= (1 << 28); GDIR &= ~(1 << 28); Sleep (150) }
      Method (_OFF) { GDAT &= ~(1 << 28) }
    }

    //
    // This controller drives the SDIO Wi-Fi.
    // It can only run at DDR50 with fixed signaling voltage, so there's no
    // need to apply most of the workarounds above.
    //
    Device (SDC1) {
      Name (_HID, "BRCM5D12")
      Name (_CID, Package () { "80860F16", "VEN_8086&DEV_0F14" })
      Name (_UID, 0x1)
      Name (_CCA, 0x0)
      Name (_PR0, Package () { WLPW })

      Method (_CRS, 0x0, Serialized) {
        Name (RBUF, ResourceTemplate () {
          QWORDMEMORY_BUF (00, ResourceConsumer)
          Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 306 }
        })
        QWORD_SET (00, BCM2712_BRCMSTB_SDIO2_HOST_BASE, BCM2712_BRCMSTB_SDIO_HOST_LENGTH, 0)
        Return (RBUF)
      }

      //
      // Only needed by Windows.
      //
      Method (_DSM, 4, Serialized) {
        // Check the UUID
        If (Arg0 == ToUUID ("f6c13ea5-65cd-461f-ab7a-29f7e8d5bd61")) {
          // Check the revision
          If (Arg1 >= 0) {
            // Check the function index
            Switch (ToInteger (Arg2)) {
              //
              // Supported functions:
              // Bit 0 - Indicates support for functions other than 0
              // Bit 8 - Indicates support for UHS-I modes
              //
              Case (0) {
                Return (Buffer () { 0x01, 0x01 }) // 0x101
              }

              //
              // Function Index 8: Supported UHS-I modes
              // Bit 0 - SDR25
              // Bit 1 - DDR50
              // Bit 2 - SDR50
              // Bit 3 - SDR104
              //
              Case (8) {
                Return (Buffer () { 0x02 }) // DDR50
              }
            } // Function index check
          } // Revision check
        } // UUID check
        Return (Buffer () { 0x0 })
      } // _DSM

      Method (_DSD, 0x0, Serialized) {
        Return (Package () {
          ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
          Package () {
            // Disable hardware retuning, SDR104, SDR50.
            Package () { "sdhci-caps-mask", (1 << 47) | (1 << 46) | (1 << 33) | (1 << 32) },
          }
        })
      } // _DSD

      //
      // Fixed CYW43455 SDIO Wi-Fi
      //
      Device (WLAN) {
        Name (_ADR, 0x1)

        Method (_RMV) {
          Return (0)
        }
      }
    } // Device (SDC1)

    // VideoCore VII GPU (V3D 7.1) + HVS/PixelValve display pipeline.
    Device (GPU0) {
      Name (_HID, "BCM2712")
      Name (_CID, "BCM2850")
      Name (_UID, 0x0)
      Name (_CCA, 0x0)

      Method (_STA) {
        Return (0xF)
      }

      Method (_CRS, 0x0, Serialized) {
        Name (RBUF, ResourceTemplate () {
          QWORDMEMORY_BUF (00, ResourceConsumer)
          QWORDMEMORY_BUF (01, ResourceConsumer)
          QWORDMEMORY_BUF (02, ResourceConsumer)
          QWORDMEMORY_BUF (03, ResourceConsumer)
          QWORDMEMORY_BUF (04, ResourceConsumer)
          QWORDMEMORY_BUF (05, ResourceConsumer)
          QWORDMEMORY_BUF (06, ResourceConsumer)
          QWORDMEMORY_BUF (07, ResourceConsumer)
          QWORDMEMORY_BUF (08, ResourceConsumer)
          QWORDMEMORY_BUF (09, ResourceConsumer)
          Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { BCM2712_V3D_CORE_INTERRUPT }
          Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { BCM2712_V3D_HUB_INTERRUPT }
          Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { BCM2712_PIXELVALVE0_INTERRUPT }
          Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { BCM2712_PIXELVALVE1_INTERRUPT }
        })
        QWORD_SET (00, BCM2712_V3D_HUB_BASE,     BCM2712_V3D_HUB_LENGTH,     0)
        QWORD_SET (01, BCM2712_V3D_CORE0_BASE,   BCM2712_V3D_CORE0_LENGTH,   0)
        QWORD_SET (02, BCM2712_V3D_SMS_BASE,     BCM2712_V3D_SMS_LENGTH,     0)
        QWORD_SET (03, BCM2712_HVS_BASE,         BCM2712_HVS_LENGTH,         0)
        QWORD_SET (04, BCM2712_HVS_IOMMU_BASE,   BCM2712_HVS_IOMMU_LENGTH,   0)
        QWORD_SET (05, BCM2712_PIXELVALVE0_BASE, BCM2712_PIXELVALVE0_LENGTH, 0)
        QWORD_SET (06, BCM2712_PIXELVALVE1_BASE, BCM2712_PIXELVALVE1_LENGTH, 0)
        QWORD_SET (07, BCM2712_MOP_BASE,         BCM2712_MOP_LENGTH,         0)
        QWORD_SET (08, BCM2712_MOPLET_BASE,      BCM2712_MOPLET_LENGTH,      0)
        QWORD_SET (09, BCM2712_DISP_INTR_BASE,   BCM2712_DISP_INTR_LENGTH,   0)
        Return (RBUF)
      }

      // DXGK power components.
      Method (PMCD, 0, Serialized) {
        Name (RBUF, Package () {
          1, // Version
          1, // Component count
          Package () {
            Package () {
              0, // Component index
              0, // Engine component
              0, // Node index

              // 9B2D1E26-1575-4747-8FC0-B9EB4BAA2D2B
              Buffer () {
                0x26, 0x1E, 0x2D, 0x9B, 0x75, 0x15, 0x47, 0x47,
                0x8f, 0xc0, 0xb9, 0xeb, 0x4b, 0xaa, 0x2d, 0x2b
              },

              "V3D_Engine_00",
              2,

              // { TransitionLatency, ResidencyRequirement, NominalPower }
              Package () {
                Package () { 0, 0, 1210000 },     // F0
                Package () { 10000, 10000, 4 },   // F1
              }
            }
          }
        })
        Return (RBUF)
      }
    } // Device (GPU0)

    // Windows can use this device for passive logical-processor idling.
    Device (PAG0) {
      Name (_HID, "ACPI000C")
      Name (_UID, 0x0)
      Name (_STA, 0xF)
      Name (_PUR, Package () { 1, 0 })

      // Retain the last OSPM response for platform diagnostics. Arg2 is the
      // four-byte count of logical processors that OSPM actually idled.
      Name (OSRC, Zero)
      Name (OSTS, Zero)
      Name (OSDT, Buffer (4) { 0, 0, 0, 0 })

      Method (_OST, 3, Serialized) {
        OSRC = Arg0
        OSTS = Arg1
        OSDT = Arg2
      }
    }

    // BCM2712 AVS temperature sensor.
    OperationRegion (
      THRM,
      SystemMemory,
      BCM2712_AVS_MONITOR_BASE + BCM2712_AVS_MONITOR_TEMP_STATUS_OFFSET,
      0x4
      )
    Field (THRM, DWordAcc, NoLock, Preserve) {
      TVAL, 32
    }

    ThermalZone (TZ00) {
      // A failed sample retains a conservative 75 C reading until valid data
      // becomes available, keeping maximum active cooling engaged.
      Name (LTMP, 3482)

      Method (_TMP, 0, Serialized) {
        Local0 = TVAL
        If (Local0 & BCM2712_AVS_MONITOR_TEMP_VALID_MASK) {
          Local1 = Local0 & BCM2712_AVS_MONITOR_TEMP_DATA_MASK
          If (Local1 > 818) {
            LTMP = 2732
          } Else {
            LTMP = ((BCM2712_AVS_MONITOR_TEMP_OFFSET_MC -
                     (Local1 * BCM2712_AVS_MONITOR_TEMP_SLOPE_MC)) / 100) + 2732
          }
        }
        Return (LTMP)
      }

      Name (_RTV, Zero)
      Name (_CRT, 3832) // 110 C
      Name (_PSV, 3532) // 80 C
      Name (_TC1, 2)
      Name (_TC2, 3)
      Name (_TSP, 10)

      // ACPI numbers stronger active cooling with the lower suffix.
      Name (_AC0, 3482) // 75 C, 250/255 PWM
      Name (_AL0, Package () { \_SB.RP1B.FN00 })
      Name (_AC1, 3407) // 67.5 C, 175/255 PWM
      Name (_AL1, Package () { \_SB.RP1B.FN01 })
      Name (_AC2, 3332) // 60 C, 125/255 PWM
      Name (_AL2, Package () { \_SB.RP1B.FN02 })
      Name (_AC3, 3232) // 50 C, 75/255 PWM
      Name (_AL3, Package () { \_SB.RP1B.FN03 })

      Name (_TZP, 10)
      Name (_TZD, Package () {
        \_SB.CPU0,
        \_SB.CPU1,
        \_SB.CPU2,
        \_SB.CPU3,
        \_SB.GPU0,
        \_SB.PAG0
      })
      Name (_STR, Unicode ("BCM2712 SoC thermal zone"))
    }

    ThermalZone (TZ01) {
      Name (LTMP, 3482)

      Method (_STA, 0, NotSerialized) {
        Return (\_SB.RP1B.ASTA)
      }

      Method (_TMP, 0, Serialized) {
        Local0 = \_SB.RP1B.RTMP ()
        If (Local0 != Ones) {
          LTMP = Local0
        }

        Return (LTMP)
      }

      Name (_TZP, 10)
      Name (_TZD, Package () { \_SB.RP1B })
      Name (_STR, Unicode ("RP1 I/O controller thermal zone"))
    }

  } // Scope (\_SB_)
} // DefinitionBlock
