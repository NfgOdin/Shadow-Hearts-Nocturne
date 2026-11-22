#include "psx/cdrom.hpp"
#include "psx/system.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

int g_failures = 0;

void check_eq(const char *label, std::uint32_t actual, std::uint32_t expected) {
    if (actual != expected) {
        std::fprintf(stderr, "[FAIL] %s: actual=0x%08X, expected=0x%08X\n", label, actual, expected);
        ++g_failures;
    }
}

void test_msf_lba_conversion() {
    // LBA 0 == 00:02:00 BCD
    std::uint8_t m = 0, s = 0, f = 0;
    psx::Cdrom::lba_to_msf(0u, m, s, f);
    check_eq("LBA 0 -> M", m, 0x00u);
    check_eq("LBA 0 -> S", s, 0x02u);
    check_eq("LBA 0 -> F", f, 0x00u);
    check_eq("00:02:00 -> LBA 0", psx::Cdrom::msf_to_lba(0x00u, 0x02u, 0x00u), 0u);

    // LBA 244392 (Disc 1 end) round-trip
    psx::Cdrom::lba_to_msf(244392u, m, s, f);
    check_eq("LBA 244392 MSF round-trip", psx::Cdrom::msf_to_lba(m, s, f), 244392u);
}

void test_4disc_manifest_commands_and_autoswap() {
    namespace fs = std::filesystem;
    const fs::path tmp_dir = fs::temp_directory_path() / "psx_disc_test_tmp";
    std::error_code ec;
    fs::remove_all(tmp_dir, ec);
    fs::create_directories(tmp_dir / "shared", ec);
    fs::create_directories(tmp_dir / "disc1", ec);

    // Write a synthetic shared FORM1 file (2048 bytes)
    {
        std::vector<std::uint8_t> form1(2048, 0x5Au);
        form1[0] = 'K'; form1[1] = 'D'; form1[2] = 'L'; form1[3] = 'K';
        std::ofstream ofs(tmp_dir / "shared" / "test_form1.bin", std::ios::binary);
        ofs.write(reinterpret_cast<const char *>(form1.data()), 2048);
    }

    // Write a synthetic RAW2352 XA/STR audio sector (Mode 2 Form 2 RealTime Audio: file=1, ch=2, submode=0x64)
    {
        std::vector<std::uint8_t> raw(2352, 0);
        raw[15] = 0x02u;
        raw[16] = 0x01u;  // file 1
        raw[17] = 0x02u;  // channel 2
        raw[18] = 0x64u;  // Form 2 + RealTime + Audio
        raw[19] = 0x01u;
        std::memcpy(raw.data() + 20, raw.data() + 16, 4);
        raw[24] = 0x99u;
        std::ofstream ofs(tmp_dir / "disc1" / "test_str.raw2352", std::ios::binary);
        ofs.write(reinterpret_cast<const char *>(raw.data()), 2352);
    }

    // Write synthetic 4-disc disc_manifest.tsv using both koudelka_prepare.py FILE lines and synthetic lines
    {
        std::ofstream mf(tmp_dir / "disc_manifest.tsv");
        mf << "# Synthetic Koudelka 4-disc manifest\n";
        mf << "DISC\t1\tSLUS_01051\tSLUS_010.51\t1000\t0\t\n";
        mf << "DISC\t2\tSLUS_01100\tSLUS_011.00\t1000\t0\t\n";
        mf << "DISC\t3\tSLUS_01101\tSLUS_011.01\t1000\t0\t\n";
        mf << "DISC\t4\tSLUS_01102\tSLUS_011.02\t1000\t0\t\n";
        mf << "FILE\t1\t20\t1\t2048\tF1\tSLUS_010.51\tshared/test_form1.bin\t00\n";
        mf << "FILE\t1\t30\t1\t2352\tRAW2352\tMOVIE.STR\tdisc1/test_str.raw2352\t00\n";
        mf << "FILE\t1\t40\t1\t2048\tF1\tDISK/DISK3.TX8\tshared/test_form1.bin\t00\n";
        mf << "FILE\t2\t20\t5\t10240\tFILL:FF\tDUMMY.DAT\t-\t00\n";
    }

    psx::System sys;
    std::string err;
    if (!sys.cdrom.load_manifest(tmp_dir.string(), &err)) {
        std::fprintf(stderr, "[FAIL] load_manifest failed: %s\n", err.c_str());
        ++g_failures;
        return;
    }

    check_eq("Disc count == 4", static_cast<std::uint32_t>(sys.cdrom.disc_count()), 4u);
    check_eq("Active disc == 1", static_cast<std::uint32_t>(sys.cdrom.active_disc()), 1u);

    // Verify direct sector read of FORM1 (LBA 20 on Disc 1)
    std::uint8_t user[2048] = {};
    check_eq("Read Disc 1 LBA 20 ok", sys.cdrom.read_sector_user2048(1, 20u, user) ? 1u : 0u, 1u);
    check_eq("Disc 1 LBA 20 magic[0]", user[0], 'K');
    check_eq("Disc 1 LBA 20 magic[3]", user[3], 'K');

    // Verify CD-ROM register protocol: Test(0x20) -> 95/05/16 C1
    sys.cdrom.write_reg(sys, 0, 0);       // Index 0
    sys.cdrom.write_reg(sys, 2, 0x20u);   // Param = 0x20
    sys.cdrom.write_reg(sys, 1, 0x19u);   // Cmd = Test
    sys.cdrom.tick(sys, 20000u);
    check_eq("Test(0x20) byte 0", sys.cdrom.read_reg(1), 0x95u);
    check_eq("Test(0x20) byte 1", sys.cdrom.read_reg(1), 0x05u);
    check_eq("Test(0x20) byte 2", sys.cdrom.read_reg(1), 0x16u);
    check_eq("Test(0x20) byte 3", sys.cdrom.read_reg(1), 0xC1u);
    // Ack IRQ
    sys.cdrom.write_reg(sys, 0, 1);
    sys.cdrom.write_reg(sys, 3, 0x1Fu);

    // Verify XA-ADPCM routing on LBA 30 when Setmode(0x48) + Setfilter(1, 2) are active
    int xa_hits = 0;
    sys.cdrom.set_xa_callback([&](const std::uint8_t *raw2352, std::uint8_t file, std::uint8_t ch) {
        if (file == 1u && ch == 2u && raw2352[24] == 0x99u) ++xa_hits;
    });

    sys.cdrom.write_reg(sys, 0, 0);
    sys.cdrom.write_reg(sys, 2, 0x48u);   // XA-ADPCM + XA-Filter
    sys.cdrom.write_reg(sys, 1, 0x0Eu);   // Setmode
    sys.cdrom.tick(sys, 20000u);
    sys.cdrom.write_reg(sys, 0, 1); sys.cdrom.write_reg(sys, 3, 0x1Fu);

    sys.cdrom.write_reg(sys, 0, 0);
    sys.cdrom.write_reg(sys, 2, 0x01u);
    sys.cdrom.write_reg(sys, 2, 0x02u);
    sys.cdrom.write_reg(sys, 1, 0x0Du);   // Setfilter(1, 2)
    sys.cdrom.tick(sys, 20000u);
    sys.cdrom.write_reg(sys, 0, 1); sys.cdrom.write_reg(sys, 3, 0x1Fu);

    std::uint8_t m = 0, s = 0, f = 0;
    psx::Cdrom::lba_to_msf(30u, m, s, f);
    sys.cdrom.write_reg(sys, 0, 0);
    sys.cdrom.write_reg(sys, 2, m);
    sys.cdrom.write_reg(sys, 2, s);
    sys.cdrom.write_reg(sys, 2, f);
    sys.cdrom.write_reg(sys, 1, 0x02u);   // Setloc(LBA 30)
    sys.cdrom.tick(sys, 20000u);
    sys.cdrom.write_reg(sys, 0, 1); sys.cdrom.write_reg(sys, 3, 0x1Fu);

    sys.cdrom.write_reg(sys, 0, 0);
    sys.cdrom.write_reg(sys, 1, 0x1Bu);   // ReadS
    sys.cdrom.tick(sys, 20000u);
    sys.cdrom.write_reg(sys, 0, 1); sys.cdrom.write_reg(sys, 3, 0x1Fu);
    sys.cdrom.tick(sys, 500000u);         // Deliver LBA 30 to XA callback (1x = 451,584 cyc)
    check_eq("XA audio sector routed to callback", static_cast<std::uint32_t>(xa_hits), 1u);

    // Verify 4-disc swap (Disc 1 -> Disc 2)
    check_eq("Swap to Disc 2 initiated", sys.cdrom.swap_disc(2) ? 1u : 0u, 1u);
    check_eq("Lid open during swap", sys.cdrom.lid_open() ? 1u : 0u, 1u);
    sys.cdrom.tick(sys, 1500000u);
    check_eq("Lid closed after swap duration", sys.cdrom.lid_open() ? 1u : 0u, 0u);
    check_eq("Active disc is now Disc 2", static_cast<std::uint32_t>(sys.cdrom.active_disc()), 2u);

    // Verify Disc 2 FILL sector read (LBA 22 filled with 0xFF)
    check_eq("Read Disc 2 FILL sector", sys.cdrom.read_sector_user2048(2, 22u, user) ? 1u : 0u, 1u);
    check_eq("Disc 2 FILL byte == 0xFF", user[100], 0xFFu);

    // Regression: spinning past an earlier file into DISK/DISK3.TX8 (LBA 39 -> 40) must NOT trigger a false swap!
    sys.cdrom.set_active_disc_immediate(1);
    psx::Cdrom::lba_to_msf(39u, m, s, f);
    sys.cdrom.write_reg(sys, 0, 0);
    sys.cdrom.write_reg(sys, 2, m);
    sys.cdrom.write_reg(sys, 2, s);
    sys.cdrom.write_reg(sys, 2, f);
    sys.cdrom.write_reg(sys, 1, 0x02u);   // Setloc(LBA 39, before DISK3.TX8)
    sys.cdrom.tick(sys, 20000u);
    sys.cdrom.write_reg(sys, 0, 1); sys.cdrom.write_reg(sys, 3, 0x1Fu);
    sys.cdrom.write_reg(sys, 0, 0);
    sys.cdrom.write_reg(sys, 1, 0x06u);   // ReadN
    sys.cdrom.tick(sys, 20000u);
    sys.cdrom.write_reg(sys, 0, 1); sys.cdrom.write_reg(sys, 3, 0x1Fu);
    sys.cdrom.tick(sys, 500000u);         // LBA 39
    sys.cdrom.write_reg(sys, 0, 1); sys.cdrom.write_reg(sys, 3, 0x1Fu);
    sys.cdrom.tick(sys, 500000u);         // Spins into LBA 40 (DISK/DISK3.TX8)
    sys.cdrom.write_reg(sys, 0, 1); sys.cdrom.write_reg(sys, 3, 0x1Fu);
    sys.cdrom.write_reg(sys, 0, 0);
    sys.cdrom.write_reg(sys, 1, 0x09u);   // Pause
    sys.cdrom.tick(sys, 1500000u);
    check_eq("No false disc-swap when spinning past earlier LBA", static_cast<std::uint32_t>(sys.cdrom.active_disc()), 1u);

    // Switch back to Disc 1 and test automatic disc swap when explicitly seeking to DISK/DISK3.TX8 (LBA 40) + Pause
    sys.cdrom.set_active_disc_immediate(1);
    psx::Cdrom::lba_to_msf(40u, m, s, f);
    sys.cdrom.write_reg(sys, 0, 0);
    sys.cdrom.write_reg(sys, 2, m);
    sys.cdrom.write_reg(sys, 2, s);
    sys.cdrom.write_reg(sys, 2, f);
    sys.cdrom.write_reg(sys, 1, 0x02u);   // Setloc(LBA 40)
    sys.cdrom.tick(sys, 20000u);
    sys.cdrom.write_reg(sys, 0, 1); sys.cdrom.write_reg(sys, 3, 0x1Fu);

    sys.cdrom.write_reg(sys, 0, 0);
    sys.cdrom.write_reg(sys, 1, 0x06u);   // ReadN
    sys.cdrom.tick(sys, 20000u);
    sys.cdrom.write_reg(sys, 0, 1); sys.cdrom.write_reg(sys, 3, 0x1Fu);
    sys.cdrom.tick(sys, 500000u);         // Deliver LBA 40 (DISK/DISK3.TX8) (1x = 451,584 cyc)
    sys.cdrom.write_reg(sys, 0, 1); sys.cdrom.write_reg(sys, 3, 0x1Fu);

    sys.cdrom.write_reg(sys, 0, 0);
    sys.cdrom.write_reg(sys, 1, 0x09u);   // Pause -> triggers queued swap to Disc 3!
    sys.cdrom.tick(sys, 1500000u);
    check_eq("Auto-swapped to Disc 3 after reading DISK/DISK3.TX8", static_cast<std::uint32_t>(sys.cdrom.active_disc()), 3u);

    fs::remove_all(tmp_dir, ec);
}

}  // namespace

int main() {
    test_msf_lba_conversion();
    test_4disc_manifest_commands_and_autoswap();

    if (g_failures != 0) {
        std::fprintf(stderr, "psxdisctest FAILED (%d failure(s))\n", g_failures);
        return 1;
    }
    std::printf("psxdisctest PASSED (MSF/LBA, 4-disc manifest, FORM1/RAW2352/FILL, XA filter routing, auto disc-swap verified)\n");
    return 0;
}
