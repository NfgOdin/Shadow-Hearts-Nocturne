#include "psx/gte.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void check_eq(const char *label, std::uint32_t actual, std::uint32_t expected) {
    if (actual != expected) {
        std::fprintf(stderr, "[FAIL] %s: actual=0x%08X (%d), expected=0x%08X (%d)\n",
                     label, actual, static_cast<int>(actual), expected, static_cast<int>(expected));
        ++g_failures;
    }
}

void test_registers_and_special_io() {
    psx::Gte gte;
    gte.reset();

    // Test LZCS / LZCR leading bit counter
    gte.mtc2(30, 0x00100000u);
    check_eq("LZCR(0x00100000)", gte.mfc2(31), 11u);
    gte.mtc2(30, 0xFFF00000u);
    check_eq("LZCR(0xFFF00000)", gte.mfc2(31), 12u);
    gte.mtc2(30, 0x00000000u);
    check_eq("LZCR(0)", gte.mfc2(31), 32u);
    gte.mtc2(30, 0xFFFFFFFFu);
    check_eq("LZCR(-1)", gte.mfc2(31), 32u);

    // Test IRGB / ORGB conversion
    gte.mtc2(28, 0x7FFFu);  // R=31, G=31, B=31 -> IR1..3 = 31*128 = 3968
    check_eq("IRGB->IR1", gte.mfc2(9), 3968u);
    check_eq("IRGB->IR2", gte.mfc2(10), 3968u);
    check_eq("IRGB->IR3", gte.mfc2(11), 3968u);
    check_eq("ORGB readback", gte.mfc2(29), 0x7FFFu);

    // Test SXYP FIFO push via MTC2(15)
    gte.mtc2(15, 0x00100020u);
    gte.mtc2(15, 0x00300040u);
    gte.mtc2(15, 0x00500060u);
    check_eq("SXY0 after 3 pushes", gte.mfc2(12), 0x00100020u);
    check_eq("SXY1 after 3 pushes", gte.mfc2(13), 0x00300040u);
    check_eq("SXY2 after 3 pushes", gte.mfc2(14), 0x00500060u);
    check_eq("SXYP mirror of SXY2", gte.mfc2(15), 0x00500060u);

    // Test FLAG bit 31 summary bit
    gte.ctc2(31, 0x00040000u);  // bit 18 triggers bit 31 summary
    check_eq("FLAG bit 31 summary active", gte.cfc2(31), 0x80040000u);
    gte.ctc2(31, 0x00001000u);  // bit 12 does NOT trigger bit 31 summary
    check_eq("FLAG bit 12 no summary", gte.cfc2(31), 0x00001000u);
}

void test_unr_division_and_rtps() {
    psx::Gte gte;
    gte.reset();

    bool ovf = false;
    const std::uint32_t d1 = psx::Gte::divide_unr(256, 512, ovf);
    check_eq("UNR(256/512) ovf", ovf ? 1u : 0u, 0u);
    check_eq("UNR(256/512) val", d1, 0x8000u);

    const std::uint32_t d2 = psx::Gte::divide_unr(1000, 400, ovf);
    check_eq("UNR(1000/400) ovf (H >= 2*SZ3)", ovf ? 1u : 0u, 1u);
    check_eq("UNR(1000/400) clamped", d2, 0x1FFFFu);

    // Identity rotation matrix (1.0 in 1.3.12 fixed point = 0x1000 = 4096)
    gte.ctc2(0, 0x00001000u);  // RT11=4096, RT12=0
    gte.ctc2(1, 0x00000000u);  // RT13=0, RT21=0
    gte.ctc2(2, 0x00001000u);  // RT22=4096, RT23=0
    gte.ctc2(3, 0x00000000u);  // RT31=0, RT32=0
    gte.ctc2(4, 0x00001000u);  // RT33=4096
    gte.ctc2(5, 0u);           // TRX=0
    gte.ctc2(6, 0u);           // TRY=0
    gte.ctc2(7, 512u);         // TRZ=512
    gte.ctc2(24, 160 << 16);   // OFX = 160.0
    gte.ctc2(25, 120 << 16);   // OFY = 120.0
    gte.ctc2(26, 256u);        // H = 256

    // Vertex V0 = (40, -20, 0) -> after TRZ=512, SZ3=512, H/SZ3 = 0.5 -> SX = 160 + 20 = 180, SY = 120 - 10 = 110
    gte.mtc2(0, (static_cast<std::uint32_t>(static_cast<std::uint16_t>(-20)) << 16) | 40u);
    gte.mtc2(1, 0u);
    gte.execute(0x0180001u);  // RTPS (sf=1, lm=0)

    check_eq("RTPS SZ3", gte.mfc2(19), 512u);
    check_eq("RTPS SX2", static_cast<std::uint32_t>(gte.sx[2]), 180u);
    check_eq("RTPS SY2", static_cast<std::uint32_t>(gte.sy[2]), 110u);
    check_eq("RTPS FLAG clean", gte.cfc2(31), 0u);
}

void test_nclip_avsz_mvmva_sqr_op() {
    psx::Gte gte;
    gte.reset();

    // Triangle (0,0), (10,0), (0,20) -> NCLIP = 0*0 + 10*20 + 0*0 - 0*20 - 10*0 - 0*0 = +200
    gte.mtc2(12, 0x00000000u);
    gte.mtc2(13, 0x0000000Au);
    gte.mtc2(14, (20u << 16) | 0u);
    gte.execute(0x0000006u);  // NCLIP
    check_eq("NCLIP Winding +200", gte.mfc2(24), 200u);

    // AVSZ3 & AVSZ4
    gte.mtc2(16, 100u);
    gte.mtc2(17, 200u);
    gte.mtc2(18, 300u);
    gte.mtc2(19, 400u);
    gte.ctc2(29, 1365u);  // ~4096/3
    gte.ctc2(30, 1024u);  // 4096/4
    gte.execute(0x000002Du);  // AVSZ3: (200+300+400)*1365 >> 12 = 299
    check_eq("AVSZ3 OTZ", gte.mfc2(7), 299u);
    gte.execute(0x000002Eu);  // AVSZ4: (100+200+300+400)*1024 >> 12 = 250
    check_eq("AVSZ4 OTZ", gte.mfc2(7), 250u);

    // SQR(sf=0)
    gte.mtc2(9, 12u);
    gte.mtc2(10, -5 & 0xFFFFu);
    gte.mtc2(11, 100u);
    gte.execute(0x0000028u);  // SQR(sf=0)
    check_eq("SQR MAC1", gte.mfc2(25), 144u);
    check_eq("SQR MAC2", gte.mfc2(26), 25u);
    check_eq("SQR MAC3", gte.mfc2(27), 10000u);

    // OP(sf=0): diagonal RT=(2, 3, 4), IR=(5, 6, 7)
    // MAC1 = IR3*D2 - IR2*D3 = 7*3 - 6*4 = -3
    // MAC2 = IR1*D3 - IR3*D1 = 5*4 - 7*2 = 6
    // MAC3 = IR2*D1 - IR1*D2 = 6*2 - 5*3 = -3
    gte.ctc2(0, 2u);
    gte.ctc2(2, 3u);
    gte.ctc2(4, 4u);
    gte.mtc2(9, 5u);
    gte.mtc2(10, 6u);
    gte.mtc2(11, 7u);
    gte.execute(0x000000Cu);  // OP(sf=0, lm=0)
    check_eq("OP MAC1", gte.mfc2(25), static_cast<std::uint32_t>(-3));
    check_eq("OP MAC2", gte.mfc2(26), 6u);
    check_eq("OP MAC3", gte.mfc2(27), static_cast<std::uint32_t>(-3));
}

}  // namespace

int main() {
    test_registers_and_special_io();
    test_unr_division_and_rtps();
    test_nclip_avsz_mvmva_sqr_op();

    if (g_failures != 0) {
        std::fprintf(stderr, "psxgtetest FAILED (%d failure(s))\n", g_failures);
        return 1;
    }
    std::printf("psxgtetest PASSED (all GTE registers, UNR division, RTPS, NCLIP, AVSZ3/4, SQR, OP verified)\n");
    return 0;
}
