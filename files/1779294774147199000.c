#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <time.h>
#include "libsmn.h"
#include "phy_hw_map.h"

#define PMARGIN_VERSION "1.1.0"
#ifndef BUILD_TIME
#define BUILD_TIME __DATE__ " " __TIME__
#endif
#ifndef GIT_INFO
#define GIT_INFO "unknown"
#endif 

#define PHY_BASE      0x11E00000
#define PHY_STRIDE    0x20000
#define LANE_STRIDE   0x400
#define MAX_DIES 16
#define MAX_PHYS 4
#define MAX_LANES 4
#define MAX_TOTAL_LANES (MAX_PHYS * MAX_LANES)

#define CALC_SMN_ADDR(phy, lane, offset) (PHY_BASE + (PHY_STRIDE * (phy)) + (LANE_STRIDE * (lane)) + (offset))

static int g_verbose = 0;
static int g_dual_eye = 0;
#define DBG(...) do { if (g_verbose) printf(__VA_ARGS__); } while(0)

static float eye_width_ui[MAX_DIES][MAX_TOTAL_LANES], safe_iq_min_ui[MAX_DIES][MAX_TOTAL_LANES], safe_iq_max_ui[MAX_DIES][MAX_TOTAL_LANES];
static float eye_height_mv[MAX_DIES][MAX_TOTAL_LANES], safe_vdac_min_mv[MAX_DIES][MAX_TOTAL_LANES], safe_vdac_max_mv[MAX_DIES][MAX_TOTAL_LANES];
static int timing_left[MAX_DIES][MAX_TOTAL_LANES], timing_right[MAX_DIES][MAX_TOTAL_LANES];
static int voltage_down[MAX_DIES][MAX_TOTAL_LANES], voltage_up[MAX_DIES][MAX_TOTAL_LANES];
static int lane_status[MAX_DIES][MAX_TOTAL_LANES];

// BUG FIX: 原来这些数组只有 [MAX_TOTAL_LANES] 一维，多 die 并行(-p)时各线程写同一下标导致数据竞争。
// 现改为 [MAX_DIES][MAX_TOTAL_LANES]，每个 die 独立存储，消除并行写冲突。
static uint16_t orig_ana_rx_vco_ovrd_out_1[MAX_DIES][MAX_TOTAL_LANES];
static uint16_t orig_ana_rx_ctl_ovrd_out[MAX_DIES][MAX_TOTAL_LANES];
static uint16_t orig_fsm_val[MAX_DIES][MAX_TOTAL_LANES];

static int cal_slc_evn_hi_code[MAX_DIES][MAX_TOTAL_LANES], cal_slc_evn_lo_code[MAX_DIES][MAX_TOTAL_LANES];
static int cal_slc_odd_hi_code[MAX_DIES][MAX_TOTAL_LANES], cal_slc_odd_lo_code[MAX_DIES][MAX_TOTAL_LANES];
// BUG FIX: dfe_tap1_code 原为一维数组，PrintAllResults 中用 dfe_tap1_code[l] 打印所有 die 的结果，
// 但并行模式下最后完成的 die 会覆盖前面 die 的值，导致 tap1 显示错误。
// 改为二维后，每个 die 的 tap1 独立存储和打印。
static int dfe_tap1_code[MAX_DIES][MAX_TOTAL_LANES];
static int cal_slc_evn_hi_tap1_code[MAX_DIES][MAX_TOTAL_LANES], cal_slc_evn_lo_tap1_code[MAX_DIES][MAX_TOTAL_LANES];
static int cal_slc_odd_hi_tap1_code[MAX_DIES][MAX_TOTAL_LANES], cal_slc_odd_lo_tap1_code[MAX_DIES][MAX_TOTAL_LANES];
static int pmix_code_space[80], phs_code_space[80], iqc_code_space[80];
static int g_rx_rate[MAX_DIES];
static int g_rx_dfe_byp[MAX_DIES];

uint32_t read16_smn_reg(uint32_t node_id, uint32_t addr)
{
    uint32_t value = 0;
    SmnRead32(node_id, addr & 0xFFFFFFFC, &value);
    if (0x2 == (addr & 0x3)) 
    {
        value = (value >> 16);
    }
    return value & 0x0000FFFF;
}

void write16_smn_reg(uint32_t node_id, uint32_t addr, uint32_t value)
{
    uint32_t read_value = 0;
    SmnRead32(node_id, addr & 0xFFFFFFFC, &read_value);
    value = value & 0x0000FFFF;
    if (0x2 == (addr & 0x3)) 
    {
        read_value = read_value & 0x0000FFFF;
        read_value = read_value | (value << 16);
    }
    else 
    {
        read_value = read_value & 0xFFFF0000;
        read_value = read_value | value;
    }
    SmnWrite32(node_id, addr & 0xFFFFFFFC, read_value);
}

uint32_t PhyRead16(uint32_t die, int phy, int lane, uint32_t offset) {
    uint32_t addr = CALC_SMN_ADDR(phy, lane, offset);
    return read16_smn_reg(die, addr);
}

void PhyWrite16(uint32_t die, int phy, int lane, uint32_t offset, uint32_t val) {
    uint32_t addr = CALC_SMN_ADDR(phy, lane, offset);
    write16_smn_reg(die, addr, val);
    usleep(5); 
}

typedef struct {
    int phs_step;
    int dac_step;
    int dac_step_coarse;
    int rx_rate;
    int rx_dfe_byp; 
    int current_iqc_code; 
    int base_dac_evn;  // single: average of 4 slicers for DFE mode, or single for bypass mode
    int base_dac_odd;  // single: average of 4 slicers for DFE mode, or single for bypass mode
    int base_dac_evn_hi, base_dac_evn_lo, base_dac_odd_hi, base_dac_odd_lo; // 4 individual slicers (DFE mode)
    int base_dac_evn_0x;
    int base_dac_odd_0x;
    uint32_t reg_iqc_ovrd; 
    uint32_t reg_iqc_clk;
    int scope_evn_addr;
    int scope_odd_addr;
    int scope_dcc_diff; 
    int nyquist;
    int prev_bit;
    int meas_errs;
    int meas_errs_coarse;
    int stat_len;
    float dac_range;
    int debug;
    int pcs_read; 
} MarginParams;

void Matlab_InitCodeSpaces() {
    int pmix_raw[] = {
        78,76,74,72,70,68,66,64,48,50,52,54,56,58,60,62,46,44,42,40,38,36,34,32,
        16,18,20,22,24,26,28,30,14,12,10,8,6,4,2,0,144,146,148,150,152,154,156,
        158,142,140,138,136,134,132,130,128,112,114,116,118,120,122,124,126,
        110,108,106,104,102,100,98,96,80,82,84,86,88,90,92,94
    };
    memcpy(pmix_code_space, pmix_raw, sizeof(pmix_raw));
    
    int phs_raw[] = {
        0,1,2,3,4,5,6,7,7,8,9,10,11,12,13,14,14,15,16,17,18,19,20,21,21,22,23,
        24,25,26,27,28,28,29,30,31,32,33,34,35,35,36,37,38,39,40,41,42,42,43,
        44,45,46,47,48,49,49,50,51,52,53,54,55,56,56,57,58,59,60,61,62,63,63,
        64,65,66,67,68,69,0
    };
    memcpy(phs_code_space, phs_raw, sizeof(phs_raw));
    
    int iqc_raw[] = {
        0,1,2,3,4,5,6,8,9,10,11,12,13,14,16,17,18,19,20,21,22,24,25,26,27,28,
        29,30,32,33,34,35,36,37,38,40,41,42,43,44,45,46,48,49,50,51,52,53,54,
        56,57,58,59,60,61,62,64,65,66,67,68,69,70,72,73,74,75,76,77,78
    };
    memset(iqc_code_space, 0, sizeof(iqc_code_space));
    memcpy(iqc_code_space, iqc_raw, sizeof(iqc_raw));
}

void Matlab_InitStatsBlockStatic(uint32_t die, int phy, int lane, MarginParams *p) {
    uint16_t ctl1_val = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL1);
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL1, ctl1_val & ~(1<<0)); 
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL2, 0); 
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL3, 0); 
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL4, 0); 
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL5, 0); 
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL6, 0); 
    
    LANE0_RX_STAT_STAT_CTL1_Typedef ctl1;
    ctl1.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL1);
    ctl1.bits.stat_clk_en = 1;
    ctl1.bits.stat_cnt_3_en = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL1, ctl1.value);
    ctl1.bits.vld_loss_clr = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL1, ctl1.value);
    usleep(5); 
    ctl1.bits.vld_loss_clr = 0; 
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL1, ctl1.value);
    
    LANE0_RX_STAT_STAT_CTL2_Typedef ctl2;
    ctl2.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL2);
    ctl2.bits.invert_corr_vga_en = 1; 
    ctl2.bits.scope_fifo_rst_en = 1;
    ctl2.bits.enable_auto_glcm = 1;
    ctl2.bits.sc_pause = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL2, ctl2.value);
    
    LANE0_RX_STAT_STAT_CTL0_Typedef ctl0;
    ctl0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL0);
    ctl0.bits.sc_timer_mode = 0;
    ctl0.bits.stat_rxclk_sel = 0;
    ctl0.bits.stat_src_sel = 0;
    ctl0.bits.corr_mode_en = 0;
    ctl0.bits.skip_en = 0;
    ctl0.bits.stat_shft_sel = 0;
    ctl0.bits.corr_src_sel = 0;
    ctl0.bits.corr_shft_sel = 0;
    ctl0.bits.corr_shft_sel_vga = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL0, ctl0.value);

    LANE0_RX_STAT_CAL_COMP_CLK_CTL_Typedef comp_clk;
    comp_clk.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_CAL_COMP_CLK_CTL);
    comp_clk.bits.ref_div_cnt = 3;
    comp_clk.bits.prechrge_cnt = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_CAL_COMP_CLK_CTL, comp_clk.value);
}

static int Matlab_RunStatsBlockOnce(uint32_t die, int phy, int lane, int stat_len) {
    uint32_t sc1_ld_val = stat_len - 1;

    LANE0_RX_STAT_LD_VAL_1_Typedef ld1_ctl;
    ld1_ctl.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_LD_VAL_1);
    ld1_ctl.bits.sc1_ld_val = sc1_ld_val;
    ld1_ctl.bits.sc1_start = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_LD_VAL_1, ld1_ctl.value);
    usleep(5);
    ld1_ctl.bits.sc1_start = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_LD_VAL_1, ld1_ctl.value);

    int timeout = 10000;
    while(timeout > 0) {
        uint16_t smpl_val = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_SMPL_CNT1);
        if (smpl_val == 0xFFFF) return -2;
        if (smpl_val & 0x8000) break;
        usleep(10);
        timeout--;
    }
    if (timeout <= 0) return -1;

    LANE0_RX_STAT_STAT_CNT_3_Typedef cnt3;
    cnt3.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CNT_3);
    if (cnt3.value == 0xFFFF) return -2;

    return cnt3.bits.stat_cnt_3;
}

int Matlab_RunStatsBlock(uint32_t die, int phy, int lane, int stat_len) {
    for (int retry = 0; retry < 3; retry++) {
        int result = Matlab_RunStatsBlockOnce(die, phy, lane, stat_len);
        if (result >= 0) return result;
        usleep(50);
    }
    return -1;
}

int Matlab_MarginErrorCheck(uint32_t die, int phy, int lane, int meas_time_ms, int meas_errs) {
    int status = 0;

    LANE0_RX_STAT_STAT_STOP_Typedef stop_ctl;
    stop_ctl.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_STAT_STOP);
    stop_ctl.bits.sc1_stop = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_STAT_STOP, stop_ctl.value);
    usleep(2);
    stop_ctl.bits.sc1_stop = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_STAT_STOP, stop_ctl.value);

    LANE0_RX_STAT_STAT_CTL2_Typedef ctl2;
    ctl2.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL2);
    ctl2.bits.sc_pause = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL2, ctl2.value);

    LANE0_RX_STAT_LD_VAL_1_Typedef ld1_ctl;
    ld1_ctl.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_LD_VAL_1);
    ld1_ctl.bits.sc1_start = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_LD_VAL_1, ld1_ctl.value);
    usleep(2);
    ld1_ctl.bits.sc1_start = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_LD_VAL_1, ld1_ctl.value);

    ctl2.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL2);
    ctl2.bits.sc_pause = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL2, ctl2.value);
    usleep(100);

    int timeout = meas_time_ms;
    while(timeout > 0) {
        LANE0_RX_STAT_STAT_CNT_3_Typedef cnt3;
        cnt3.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CNT_3);
        if (cnt3.value == 0xFFFF) { status = 1; break; }
        if (cnt3.bits.stat_cnt_3 > meas_errs) { status = 1; break; }
        usleep(1000);
        timeout -= 1;
    }

    stop_ctl.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_STAT_STOP);
    stop_ctl.bits.sc1_stop = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_STAT_STOP, stop_ctl.value);
    usleep(2);
    stop_ctl.bits.sc1_stop = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_STAT_STOP, stop_ctl.value);

    return status;
}

int Matlab_MarginErrorCheckCoarse(uint32_t die, int phy, int lane, int meas_errs) {
    int err_cnt = Matlab_RunStatsBlock(die, phy, lane, 2048);
    if (err_cnt < 0 || err_cnt > meas_errs) {
        return 1;
    }
    return 0;
}

void Matlab_ConfigStatsCoarse(uint32_t die, int phy, int lane) {
    LANE0_RX_STAT_LD_VAL_1_Typedef ld1;
    ld1.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_LD_VAL_1);
    ld1.bits.sc1_ld_val = 32000 - 1;
    ld1.bits.sc1_start = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_LD_VAL_1, ld1.value);

    LANE0_RX_STAT_STAT_CTL0_Typedef ctl0;
    ctl0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL0);
    ctl0.bits.sc_timer_mode = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL0, ctl0.value);

    LANE0_RX_STAT_STAT_CTL2_Typedef ctl2;
    ctl2.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL2);
    ctl2.bits.disable_sample_count = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL2, ctl2.value);
}

void Matlab_ConfigStatsFine(uint32_t die, int phy, int lane) {
    LANE0_RX_STAT_STAT_STOP_Typedef stop;
    stop.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_STAT_STOP);
    stop.bits.sc1_stop = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_STAT_STOP, stop.value);
    usleep(2);
    stop.bits.sc1_stop = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_STAT_STOP, stop.value);

    LANE0_RX_STAT_LD_VAL_1_Typedef ld1;
    ld1.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_LD_VAL_1);
    ld1.bits.sc1_ld_val = 2047;
    ld1.bits.sc1_start = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_LD_VAL_1, ld1.value);

    LANE0_RX_STAT_STAT_CTL0_Typedef ctl0;
    ctl0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL0);
    ctl0.bits.sc_timer_mode = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL0, ctl0.value);

    LANE0_RX_STAT_STAT_CTL2_Typedef ctl2;
    ctl2.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL2);
    ctl2.bits.disable_sample_count = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL2, ctl2.value);
}

void e32g_rx_scope_walk_pmix_steps(uint32_t die, int phy, int lane, MarginParams *p, int *current_idx, int incdec, int num_steps) {
    if (num_steps < 0) num_steps = num_steps % 80 + 80;
    for (int i = 0; i < num_steps; i++) {
        *current_idx = (*current_idx + incdec + 80) % 80;
        int pmix_val = pmix_code_space[*current_idx];

        LANE0_ANA_RX_ANA_IQC_BYP_OVRD_Typedef iqc_ovrd;
        iqc_ovrd.value = PhyRead16(die, phy, lane, p->reg_iqc_ovrd);
        iqc_ovrd.bits.val = pmix_val;
        iqc_ovrd.bits.en = 1;
        PhyWrite16(die, phy, lane, p->reg_iqc_ovrd, iqc_ovrd.value);

        LANE0_ANA_RX_ANA_IQC_BYPASS_ADJUST_CLK_Typedef iqc_clk;
        iqc_clk.value = PhyRead16(die, phy, lane, p->reg_iqc_clk);
        iqc_clk.bits.val = 1;
        PhyWrite16(die, phy, lane, p->reg_iqc_clk, iqc_clk.value);
        usleep(2);
        iqc_clk.bits.val = 0;
        PhyWrite16(die, phy, lane, p->reg_iqc_clk, iqc_clk.value);
    }
}

void e32g_rx_margin_walk_pmix_timing(uint32_t die, int phy, int lane, MarginParams *p, int *current_idx, int incdec, int num_steps) {
    if (num_steps < 0) num_steps = num_steps % 80 + 80;

    for (int i = 0; i < num_steps; i++) {
        *current_idx = (*current_idx + incdec + 80) % 80;
        int pmix_val = pmix_code_space[*current_idx];

        LANE0_ANA_RX_ANA_IQC_BYP_OVRD_Typedef iqc_ovrd;
        iqc_ovrd.value = PhyRead16(die, phy, lane, p->reg_iqc_ovrd);
        iqc_ovrd.bits.en = 1;
        iqc_ovrd.bits.val = pmix_val;
        PhyWrite16(die, phy, lane, p->reg_iqc_ovrd, iqc_ovrd.value);

        LANE0_ANA_RX_ANA_IQC_BYPASS_ADJUST_CLK_Typedef iqc_clk;
        iqc_clk.value = PhyRead16(die, phy, lane, p->reg_iqc_clk);
        iqc_clk.bits.val = 1;
        PhyWrite16(die, phy, lane, p->reg_iqc_clk, iqc_clk.value);
        usleep(2);
        iqc_clk.bits.val = 0;
        PhyWrite16(die, phy, lane, p->reg_iqc_clk, iqc_clk.value);
    }
}

int Matlab_ScopeIqcSearch(uint32_t die, int phy, int lane, MarginParams *p) {
    int num_iqc_steps = 20;
    float corr_lvl = 0.5;
    int best_corr_delta = 0x7FFFFFFF;
    int best_iqc_code = p->current_iqc_code;
    int first_iter = 1;

    for (int iter = 0; iter < num_iqc_steps; iter++) {
        int corr = Matlab_RunStatsBlock(die, phy, lane, p->stat_len);
        if (corr < 0) return -1;

        uint16_t smpl_val = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_SMPL_CNT1);
        int samp = smpl_val & 0x7FFF;

        int corr_delta = corr - (int)(samp * corr_lvl);

        if (abs(corr_delta) < abs(best_corr_delta)) {
            best_corr_delta = corr_delta;
            best_iqc_code = p->current_iqc_code;
        }

        if (!first_iter) {
            if ((corr_delta > 0 && best_corr_delta < 0) || (corr_delta < 0 && best_corr_delta > 0)) {
                best_corr_delta = corr_delta;
                best_iqc_code = p->current_iqc_code;
                break;
            }
        }
        first_iter = 0;

        int incdec = (corr_delta > 0) ? 1 : ((corr_delta < 0) ? -1 : 0);
        if (incdec == 0) break;
        e32g_rx_scope_walk_pmix_steps(die, phy, lane, p, &p->current_iqc_code, incdec, 1);
    }

    int incdec = (p->current_iqc_code > best_iqc_code) ? -1 : 1;
    int steps = abs(p->current_iqc_code - best_iqc_code);
    if (steps > 40) {
        incdec = -incdec;
        steps = 80 - steps;
    }
    e32g_rx_scope_walk_pmix_steps(die, phy, lane, p, &p->current_iqc_code, incdec, steps);

    return best_iqc_code;
}

int Matlab_ScopeDccCalibrationFull(uint32_t die, int phy, int lane, MarginParams *p) {
    DBG("\033[1;36m[STEP 1] Scope DCC Calibration (Die %u, PHY %d, Lane %d)\033[0m\n", die, phy, lane);
    
    int dcc_df_addr = p->rx_dfe_byp ? 22 : 18;
    int dcc_cm_addr = p->rx_dfe_byp ? 21 : 17;
    int dcc_df_muxa = p->rx_dfe_byp ? 30 : 30;
    int dcc_cm_muxa = p->rx_dfe_byp ? 13 : 14;
    
    LANE0_ANA_RX_DAC_CTRL_OVRD_Typedef dac_ovrd;
    dac_ovrd.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_OVRD);
    dac_ovrd.bits.rx_cal_dac_ctrl_ovrd = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_OVRD, dac_ovrd.value);
    
    LANE0_ANA_RX_CAL_0_Typedef cal0;
    cal0.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_CAL_0);
    cal0.bits.rx_ana_cal_comp_en = 1;
    cal0.bits.rx_ana_slicer_cal_en = 0;
    cal0.bits.rx_ana_cal_lpfbyp_en_ovrd_en = 1;
    cal0.bits.rx_ana_cal_lpfbyp_en = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_CAL_0, cal0.value);
    
    LANE0_ANA_RX_CAL_1_Typedef cal1;
    cal1.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_CAL_1);
    cal1.bits.rx_ana_cal_muxb_sel = 30;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_CAL_1, cal1.value);
    
    LANE0_ANA_RX_ANA_IQ_Typedef ana_iq;
    ana_iq.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_ANA_IQ);
    ana_iq.bits.sense_sel = 0;
    ana_iq.bits.sense_en = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_IQ, ana_iq.value);
    
    LANE0_ANA_RX_ANA_PHS_SAMP_SEL_Typedef phs_samp;
    phs_samp.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_ANA_PHS_SAMP_SEL);
    phs_samp.bits.val = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_PHS_SAMP_SEL, phs_samp.value);
    
    LANE0_ANA_RX_ANA_DFE_SAMP_SEL_Typedef dfe_samp;
    dfe_samp.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_ANA_DFE_SAMP_SEL);
    dfe_samp.bits.val = p->rx_dfe_byp;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_DFE_SAMP_SEL, dfe_samp.value);
    
    LANE0_ANA_RX_ANA_BYP_SAMP_SEL_Typedef byp_samp;
    byp_samp.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_ANA_BYP_SAMP_SEL);
    byp_samp.bits.val = !p->rx_dfe_byp;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_BYP_SAMP_SEL, byp_samp.value);
    
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_LD_VAL_1, p->stat_len - 1);
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_DATA_MSK, 0x0001);
    
    LANE0_RX_STAT_MATCH_CTL0_Typedef match_ctl0;
    match_ctl0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0);
    match_ctl0.bits.data_msk_19_16 = 0x0;
    match_ctl0.bits.pttrn_cr1a_4_0 = 0;
    match_ctl0.bits.pttrn_msk_cr1a_4_0 = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0, match_ctl0.value);
    
    LANE0_RX_STAT_STAT_CTL0_Typedef ctl0;
    ctl0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL0);
    ctl0.bits.sc_timer_mode = 0;
    ctl0.bits.stat_rxclk_sel = 0;
    ctl0.bits.stat_src_sel = 0;
    ctl0.bits.corr_mode_en = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL0, ctl0.value);
    
    LANE0_RX_ADPTCTL_ADPT_CFG_1_Typedef adpt_cfg1;
    adpt_cfg1.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_ADPT_CFG_1);
    adpt_cfg1.bits.adpt_rxclk_sel = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_ADPT_CFG_1, adpt_cfg1.value);
    
    LANE0_RX_ADPTCTL_SSM_SSM_CFG_0_Typedef ssm_cfg0;
    ssm_cfg0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0);
    ssm_cfg0.bits.ssm_dest_sel = 0;
    ssm_cfg0.bits.ssm_th_ofst = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0, ssm_cfg0.value);
    
    LANE0_RX_ADPTCTL_SSM_SSM_CFG_1_Typedef ssm_cfg1;
    ssm_cfg1.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_1);
    ssm_cfg1.bits.lin_step = 1;
    ssm_cfg1.bits.nun_bin_steps = 7;
    ssm_cfg1.bits.num_lin_steps = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_1, ssm_cfg1.value);
    
    LANE0_RX_ADPTCTL_SSM_SSM_CFG_2_Typedef ssm_cfg2;
    ssm_cfg2.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_2);
    ssm_cfg2.bits.disable_bin_hold = 0;
    ssm_cfg2.bits.init_bin_step = 7;
    ssm_cfg2.bits.init_dac_code = 256;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_2, ssm_cfg2.value);
    
    LANE0_RX_ADPTCTL_SSM_SSM_CFG_3_Typedef ssm_cfg3;
    ssm_cfg3.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_3);
    ssm_cfg3.bits.ssm_dir = 0;
    ssm_cfg3.bits.sticky_en = 1;
    ssm_cfg3.bits.lpfbyp_en = 1;
    ssm_cfg3.bits.ssm_init_wait = 8;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_3, ssm_cfg3.value);
    
    LANE0_RX_ADPTCTL_SSM_SSM_CFG_4_Typedef ssm_cfg4;
    ssm_cfg4.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_4);
    ssm_cfg4.bits.ssm_n_wait_lpfbyp = 8;
    ssm_cfg4.bits.ssm_n_wait = 8;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_4, ssm_cfg4.value);
    
    int cal_dcc_df_code = 256;
    int cal_dcc_cm_code = 256;
    
    LANE0_ANA_RX_DAC_CTRL_SEL_Typedef dac_sel;
    dac_sel.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_SEL);
    dac_sel.bits.rx_ana_cal_dac_ctrl_sel = dcc_df_addr;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_SEL, dac_sel.value);
    
    LANE0_ANA_RX_DAC_CTRL_Typedef dac_ctrl;
    dac_ctrl.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL);
    dac_ctrl.bits.rx_ana_cal_dac_ctrl = cal_dcc_df_code;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL, dac_ctrl.value);
    
    LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN_Typedef dac_en;
    dac_en.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN);
    dac_en.bits.rx_ana_cal_dac_ctrl_en = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN, dac_en.value);
    usleep(2);
    dac_en.bits.rx_ana_cal_dac_ctrl_en = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN, dac_en.value);
    
    dac_sel.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_SEL);
    dac_sel.bits.rx_ana_cal_dac_ctrl_sel = dcc_cm_addr;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_SEL, dac_sel.value);
    
    dac_ctrl.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL);
    dac_ctrl.bits.rx_ana_cal_dac_ctrl = cal_dcc_cm_code;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL, dac_ctrl.value);
    
    dac_en.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN);
    dac_en.bits.rx_ana_cal_dac_ctrl_en = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN, dac_en.value);
    usleep(2);
    dac_en.bits.rx_ana_cal_dac_ctrl_en = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN, dac_en.value);

    dac_ovrd.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_OVRD);
    dac_ovrd.bits.rx_cal_dac_ctrl_ovrd = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_OVRD, dac_ovrd.value);

    if (p->rx_rate <= 1) {
        cal0.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_CAL_0);
        cal0.bits.rx_ana_cal_mode = 1;
        PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_CAL_0, cal0.value);
        
        cal1.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_CAL_1);
        cal1.bits.rx_ana_cal_muxa_sel = dcc_df_muxa;
        PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_CAL_1, cal1.value);
        
        ssm_cfg0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0);
        ssm_cfg0.bits.ssm_dac_sel = dcc_df_addr;
        ssm_cfg0.bits.start_ssm = 1;
        PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0, ssm_cfg0.value);
        ssm_cfg0.bits.start_ssm = 0;
        PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0, ssm_cfg0.value);
        
        int timeout = 20000;
        LANE0_RX_ADPTCTL_SSM_FINAL_CODE_Typedef ssm_final;
        do {
            usleep(100);
            ssm_final.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_FINAL_CODE);
        } while (!ssm_final.bits.ssm_done && --timeout > 0);

        if (timeout > 0) cal_dcc_df_code = ssm_final.bits.dac_code;
        
        cal0.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_CAL_0);
        cal0.bits.rx_ana_cal_mode = 3;
        PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_CAL_0, cal0.value);
        
        cal1.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_CAL_1);
        cal1.bits.rx_ana_cal_muxa_sel = dcc_cm_muxa;
        PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_CAL_1, cal1.value);
        
        ssm_cfg0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0);
        ssm_cfg0.bits.ssm_dac_sel = dcc_cm_addr;
        ssm_cfg0.bits.start_ssm = 1;
        PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0, ssm_cfg0.value);
        ssm_cfg0.bits.start_ssm = 0;
        PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0, ssm_cfg0.value);
        
        timeout = 20000;
        do {
            usleep(100);
            ssm_final.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_FINAL_CODE);
        } while (!ssm_final.bits.ssm_done && --timeout > 0);

        if (timeout > 0) cal_dcc_cm_code = ssm_final.bits.dac_code;

        cal0.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_CAL_0);
        cal0.bits.rx_ana_cal_mode = 1;
        PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_CAL_0, cal0.value);

        cal1.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_CAL_1);
        cal1.bits.rx_ana_cal_muxa_sel = dcc_df_muxa;
        PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_CAL_1, cal1.value);

        ssm_cfg1.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_1);
        ssm_cfg1.bits.nun_bin_steps = 0;
        ssm_cfg1.bits.num_lin_steps = 8;
        PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_1, ssm_cfg1.value);

        ssm_cfg2.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_2);
        ssm_cfg2.bits.init_dac_code = cal_dcc_df_code;
        PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_2, ssm_cfg2.value);

        ssm_cfg0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0);
        ssm_cfg0.bits.ssm_dac_sel = dcc_df_addr;
        ssm_cfg0.bits.start_ssm = 1;
        PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0, ssm_cfg0.value);
        ssm_cfg0.bits.start_ssm = 0;
        PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0, ssm_cfg0.value);

        timeout = 20000;
        do {
            usleep(100);
            ssm_final.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_FINAL_CODE);
        } while (!ssm_final.bits.ssm_done && --timeout > 0);

        if (timeout > 0) cal_dcc_df_code = ssm_final.bits.dac_code;
        
        cal0.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_CAL_0);
        cal0.bits.rx_ana_cal_mode = 3;
        PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_CAL_0, cal0.value);
        
        cal1.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_CAL_1);
        cal1.bits.rx_ana_cal_muxa_sel = dcc_cm_muxa;
        PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_CAL_1, cal1.value);
        
        ssm_cfg2.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_2);
        ssm_cfg2.bits.init_dac_code = cal_dcc_cm_code;
        PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_2, ssm_cfg2.value);
        
        ssm_cfg0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0);
        ssm_cfg0.bits.ssm_dac_sel = dcc_cm_addr;
        ssm_cfg0.bits.start_ssm = 1;
        PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0, ssm_cfg0.value);
        ssm_cfg0.bits.start_ssm = 0;
        PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0, ssm_cfg0.value);

        timeout = 20000;
        do {
            usleep(100);
            ssm_final.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_FINAL_CODE);
        } while (!ssm_final.bits.ssm_done && --timeout > 0);

        if (timeout > 0) cal_dcc_cm_code = ssm_final.bits.dac_code;
    }

    dac_ovrd.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_OVRD);
    dac_ovrd.bits.rx_cal_dac_ctrl_ovrd = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_OVRD, dac_ovrd.value);

    dac_sel.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_SEL);
    dac_sel.bits.rx_ana_cal_dac_ctrl_sel = dcc_df_addr;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_SEL, dac_sel.value);
    
    dac_ctrl.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL);
    dac_ctrl.bits.rx_ana_cal_dac_ctrl = cal_dcc_df_code;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL, dac_ctrl.value);
    
    dac_en.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN);
    dac_en.bits.rx_ana_cal_dac_ctrl_en = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN, dac_en.value);
    usleep(2);
    dac_en.bits.rx_ana_cal_dac_ctrl_en = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN, dac_en.value);
    
    dac_sel.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_SEL);
    dac_sel.bits.rx_ana_cal_dac_ctrl_sel = dcc_cm_addr;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_SEL, dac_sel.value);
    
    dac_ctrl.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL);
    dac_ctrl.bits.rx_ana_cal_dac_ctrl = cal_dcc_cm_code;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL, dac_ctrl.value);
    
    dac_en.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN);
    dac_en.bits.rx_ana_cal_dac_ctrl_en = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN, dac_en.value);
    usleep(2);
    dac_en.bits.rx_ana_cal_dac_ctrl_en = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN, dac_en.value);
    
    dac_ovrd.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_OVRD);
    dac_ovrd.bits.rx_cal_dac_ctrl_ovrd = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_OVRD, dac_ovrd.value);
    
    dfe_samp.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_ANA_DFE_SAMP_SEL);
    dfe_samp.bits.val = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_DFE_SAMP_SEL, dfe_samp.value);
    
    byp_samp.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_ANA_BYP_SAMP_SEL);
    byp_samp.bits.val = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_BYP_SAMP_SEL, byp_samp.value);
    
    p->scope_dcc_diff = cal_dcc_df_code;
    DBG("\033[1;32m  --> Full DCC Calibration Done. Diff Code = %d, CM Code = %d\033[0m\n", 
           cal_dcc_df_code, cal_dcc_cm_code);
    return cal_dcc_df_code;
}

int Matlab_PatternCheck(uint32_t die, int phy, int lane, MarginParams *p, int *pttrn_evn, int *pttrn_odd) {
    DBG("\033[1;36m[STEP 2.5] Pattern Check (Die %u, PHY %d, Lane %d)\033[0m\n", die, phy, lane);
    
    int pttrn_list_full[] = {6, 4, 2, 0};
    int num_pttrns = sizeof(pttrn_list_full) / sizeof(pttrn_list_full[0]);
    *pttrn_evn = 0;
    *pttrn_odd = 0;
    
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_LD_VAL_1, 2047);
    
    LANE0_RX_STAT_MATCH_CTL0_Typedef match_ctl0;
    match_ctl0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0);
    match_ctl0.bits.pttrn_msk_cr1a_4_0 = 6;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0, match_ctl0.value);
    
    LANE0_RX_STAT_STAT_CTL0_Typedef ctl0;
    ctl0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL0);
    ctl0.bits.sc_timer_mode = 1;
    ctl0.bits.stat_rxclk_sel = 1;
    ctl0.bits.stat_src_sel = 4;
    ctl0.bits.corr_mode_en = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL0, ctl0.value);
    
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_DATA_MSK, 0xAAAA);
    match_ctl0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0);
    match_ctl0.bits.data_msk_19_16 = 0xA;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0, match_ctl0.value);
    
    DBG("  [Even Path] Checking patterns: ");
    for (int i = 0; i < num_pttrns; i++) {
        match_ctl0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0);
        match_ctl0.bits.pttrn_cr1a_4_0 = pttrn_list_full[i];
        PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0, match_ctl0.value);
        
        int err = Matlab_RunStatsBlock(die, phy, lane, 2048);
        if (err >= 0) {
            uint16_t smpl_val = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_SMPL_CNT1);
            int samp = smpl_val & 0x7FFF;
            if (samp > 0) {
                *pttrn_evn |= (1 << pttrn_list_full[i]);
                DBG("%d ", pttrn_list_full[i]);
            }
        }
    }
    DBG("\n");
    
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_DATA_MSK, 0x5555);
    match_ctl0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0);
    match_ctl0.bits.data_msk_19_16 = 0x5;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0, match_ctl0.value);
    
    DBG("  [Odd Path] Checking patterns: ");
    for (int i = 0; i < num_pttrns; i++) {
        match_ctl0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0);
        match_ctl0.bits.pttrn_cr1a_4_0 = pttrn_list_full[i];
        PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0, match_ctl0.value);
        
        int err = Matlab_RunStatsBlock(die, phy, lane, 2048);
        if (err >= 0) {
            uint16_t smpl_val = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_SMPL_CNT1);
            int samp = smpl_val & 0x7FFF;
            if (samp > 0) {
                *pttrn_odd |= (1 << pttrn_list_full[i]);
                DBG("%d ", pttrn_list_full[i]);
            }
        }
    }
    DBG("\n");
    
    int evn_count = __builtin_popcount(*pttrn_evn);
    int odd_count = __builtin_popcount(*pttrn_odd);
    p->nyquist = (evn_count == 1 || odd_count == 1);
    
    DBG("\033[1;32m  --> Pattern Check Done. Even: 0x%X, Odd: 0x%X, Nyquist: %s\033[0m\n",
           *pttrn_evn, *pttrn_odd, p->nyquist ? "Yes" : "No");
    
    return 0;
}

int Matlab_ScopeBitAlignmentFull(uint32_t die, int phy, int lane, MarginParams *p) {
    DBG("\033[1;36m[STEP 2] Bit Alignment & IQC Calibration (Die %u, PHY %d, Lane %d)\033[0m\n", die, phy, lane);
    
    LANE0_ANA_RX_CAL_0_Typedef cal0;
    cal0.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_CAL_0);
    cal0.bits.rx_ana_cal_comp_en = 1;
    cal0.bits.rx_ana_cal_lpfbyp_en_ovrd_en = 1;
    cal0.bits.rx_ana_cal_lpfbyp_en = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_CAL_0, cal0.value);
    
    LANE0_ANA_RX_CAL_1_Typedef cal1;
    cal1.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_CAL_1);
    cal1.bits.rx_ana_cal_muxb_sel = 31;
    cal1.bits.rx_ana_cal_muxa_sel = 31;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_CAL_1, cal1.value);
    
    LANE0_ANA_RX_ANA_IQ_Typedef ana_iq;
    ana_iq.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_ANA_IQ);
    ana_iq.bits.sense_sel = 0;
    ana_iq.bits.sense_en = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_IQ, ana_iq.value);
    
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_DATA_MSK, 0xFFFF);
    
    LANE0_RX_STAT_MATCH_CTL0_Typedef match_ctl0;
    match_ctl0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0);
    match_ctl0.bits.data_msk_19_16 = 0xF;
    match_ctl0.bits.pttrn_cr1a_4_0 = 0;
    match_ctl0.bits.pttrn_msk_cr1a_4_0 = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0, match_ctl0.value);
    
    LANE0_RX_STAT_STAT_CTL0_Typedef ctl0;
    ctl0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL0);
    ctl0.bits.sc_timer_mode = 0;
    ctl0.bits.stat_src_sel = 4;
    ctl0.bits.corr_mode_en = 1; 
    ctl0.bits.stat_rxclk_sel = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL0, ctl0.value);
    
    DBG("  [Info] Target IQC Overwrite Reg: 0x%X\n", p->reg_iqc_ovrd);
    DBG("  [Sweep] Stage 1: Coarse PMIX Code Space Sweep:\n  ");
    
    int align_iter_max = 20 * (1 << (p->rx_rate > 1 ? p->rx_rate - 1 : 0)) - 1;
    int step = (p->rx_rate == 0) ? 2 : 1;
    
    float corr_norm_vec[100];
    int align_steps = 0;

    // ROOT CAUSE FIXED: Stage 1 必须每次跨越整整 80 个 PMIX 步进（恰好 1个 UI），
    // 从而通过相位累加器驱动内部时钟位移，寻找真正正确的 Bit 眼图中央。之前仅走 step=1 步是致命伤。
    for (int idx = 0; idx <= align_iter_max; idx += step) {
        int corr = Matlab_RunStatsBlock(die, phy, lane, p->stat_len);
        if (corr == -2 || corr == -1) return -1; 
        
        uint16_t smpl_val = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_SMPL_CNT1);
        int samp = smpl_val & 0x7FFF;
        float corr_norm = (samp > 0) ? (float)corr / samp : 0;
        
        corr_norm_vec[align_steps] = corr_norm;

        DBG("[Iter:%2d Corr:%.2f] ", align_steps, corr_norm); 
        if ((align_steps + 1) % 10 == 0) DBG("\n  ");
        
        // 核心修正：步数固定写死为 80，即 2*iqc_steps_pmix
        e32g_rx_scope_walk_pmix_steps(die, phy, lane, p, &p->current_iqc_code, 1, 80);
        align_steps++;
    }
    
    float max_corr = -1.0f; 
    int corr_norm_pos = 0;
    for (int i = 0; i < align_steps; i++) {
        if (corr_norm_vec[i] > max_corr) {
            max_corr = corr_norm_vec[i];
            corr_norm_pos = i;
        }
    }
    
    // 退回到 max index 所在的 UI 相位
    int corr_norm_pos_1based = corr_norm_pos + 1;
    if (corr_norm_pos_1based < align_steps) {
        int walk_back_count = (align_steps - corr_norm_pos_1based) + 1;
        for (int w = 0; w < walk_back_count; w++) {
            e32g_rx_scope_walk_pmix_steps(die, phy, lane, p, &p->current_iqc_code, -1, 80);
        }
    }
    
    if (p->rx_rate >= 2) {
        DBG("\n\n  [Sweep] Stage 2: Fine Alignment (Right Direction):\n  ");
        float min_corr = fmaxf(0.75 * fminf((max_corr/0.9), 0.9)/0.9, 0.55);
        int align_stg2_iter = 0;
        
        for (; align_stg2_iter < 40; align_stg2_iter++) {
            int corr = Matlab_RunStatsBlock(die, phy, lane, p->stat_len);
            if (corr == -2 || corr == -1) return -1;
            
            uint16_t smpl_val = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_SMPL_CNT1);
            int samp = smpl_val & 0x7FFF;
            float corr_norm = (samp > 0) ? (float)corr / samp : 0;
            
            DBG("[Iter:%2d Corr:%.2f] ", align_stg2_iter, corr_norm);
            if ((align_stg2_iter + 1) % 10 == 0) DBG("\n  ");
            
            if (corr_norm < min_corr) break;
            e32g_rx_scope_walk_pmix_steps(die, phy, lane, p, &p->current_iqc_code, 1, 1 << p->rx_rate);
        }
        
        e32g_rx_scope_walk_pmix_steps(die, phy, lane, p, &p->current_iqc_code, -1, align_stg2_iter * (1 << p->rx_rate));
        
        DBG("\n\n  [Sweep] Stage 3: Fine Alignment (Left Direction):\n  ");
        int align_stg3_iter = 0;
        
        for (; align_stg3_iter < 40; align_stg3_iter++) {
            int corr = Matlab_RunStatsBlock(die, phy, lane, p->stat_len);
            if (corr == -2 || corr == -1) return -1;
            
            uint16_t smpl_val = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_SMPL_CNT1);
            int samp = smpl_val & 0x7FFF;
            float corr_norm = (samp > 0) ? (float)corr / samp : 0;
            
            DBG("[Iter:%2d Corr:%.2f] ", align_stg3_iter, corr_norm);
            if ((align_stg3_iter + 1) % 10 == 0) DBG("\n  ");
            
            if (corr_norm < min_corr) break;
            e32g_rx_scope_walk_pmix_steps(die, phy, lane, p, &p->current_iqc_code, -1, 1 << p->rx_rate);
        }
        
        int center_offset = (align_stg2_iter + align_stg3_iter) / 2;
        e32g_rx_scope_walk_pmix_steps(die, phy, lane, p, &p->current_iqc_code, 1, center_offset * (1 << p->rx_rate));
    }
    
    DBG("\n\n  [Sweep] Stage 4: Clock Phase Alignment:\n");
    cal0.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_CAL_0);
    cal0.bits.rx_ana_cal_mode = 0;
    cal0.bits.rx_ana_slicer_cal_en = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_CAL_0, cal0.value);
    
    ana_iq.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_ANA_IQ);
    ana_iq.bits.sense_en = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_IQ, ana_iq.value);
    
    ctl0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL0);
    ctl0.bits.stat_rxclk_sel = 0;
    ctl0.bits.stat_src_sel = 0;
    ctl0.bits.corr_mode_en = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL0, ctl0.value);
    
    if (p->rx_rate <= 1) {
        int phase_aligned_idx = Matlab_ScopeIqcSearch(die, phy, lane, p);
        if (phase_aligned_idx >= 0) {
            DBG("  --> Phase alignment done. Best index = %d\n", phase_aligned_idx);
        }
    }
    
    cal0.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_CAL_0);
    cal0.bits.rx_ana_cal_comp_en = 0;
    cal0.bits.rx_ana_cal_lpfbyp_en = 0;
    cal0.bits.rx_ana_cal_lpfbyp_en_ovrd_en = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_CAL_0, cal0.value);
    
    cal1.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_CAL_1);
    cal1.bits.rx_ana_cal_muxb_sel = 0;
    cal1.bits.rx_ana_cal_muxa_sel = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_CAL_1, cal1.value);
    
    ana_iq.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_ANA_IQ);
    ana_iq.bits.sense_sel = 0;
    ana_iq.bits.sense_en = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_IQ, ana_iq.value);
    
    DBG("\n\033[1;32m  --> Full Bit/IQC Aligned! Max Correlation = %.2f\033[0m\n", max_corr);
           
    return p->current_iqc_code;
}

int Matlab_ScopeSlicerCalibrationFull(uint32_t die, int phy, int lane, int scope_addr, uint32_t data_msk, 
                                     MarginParams *p, int is_even, int *code_0x, int pttrn_val) {
    char path_type[5] = {'E', 'v', 'e', 'n', '\0'};
    if (!is_even) {
        path_type[0] = 'O'; path_type[1] = 'd'; path_type[2] = 'd'; path_type[3] = '\0';
    }
    
    DBG("\033[1;36m[STEP 3] %s Slicer Calibration (Scope Addr: %d, DataMsk: 0x%X)\033[0m\n",
           path_type, scope_addr, data_msk);

    LANE0_RX_STAT_LD_VAL_1_Typedef ld1_slc;
    ld1_slc.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_LD_VAL_1);
    ld1_slc.bits.sc1_ld_val = p->stat_len - 1;
    ld1_slc.bits.sc1_start = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_LD_VAL_1, ld1_slc.value);

    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_DATA_MSK, data_msk);
    uint32_t cr1a_4_0_msk = (data_msk == 0xAAAA) ? 0xA : 0x5;
    
    LANE0_RX_STAT_MATCH_CTL0_Typedef match_ctl0;
    match_ctl0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0);
    match_ctl0.bits.data_msk_19_16 = cr1a_4_0_msk;
    match_ctl0.bits.pttrn_msk_cr1a_4_0 = p->rx_dfe_byp ? 2 : (p->nyquist ? 2 : 6); 
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0, match_ctl0.value);
    
    LANE0_RX_STAT_STAT_CTL0_Typedef ctl0;
    ctl0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL0);
    ctl0.bits.sc_timer_mode = 0; 
    ctl0.bits.stat_src_sel = 3;
    ctl0.bits.corr_mode_en = 0;
    ctl0.bits.stat_rxclk_sel = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL0, ctl0.value);
    
    LANE0_RX_ADPTCTL_SSM_SSM_CFG_0_Typedef ssm_cfg0;
    ssm_cfg0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0);
    ssm_cfg0.bits.ssm_dest_sel = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0, ssm_cfg0.value);
    
    LANE0_RX_ADPTCTL_ADPT_CFG_1_Typedef adpt_cfg1;
    adpt_cfg1.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_ADPT_CFG_1);
    adpt_cfg1.bits.adpt_rxclk_sel = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_ADPT_CFG_1, adpt_cfg1.value);
    
    LANE0_RX_ADPTCTL_SSM_SSM_CFG_1_Typedef ssm_cfg1;
    ssm_cfg1.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_1);
    ssm_cfg1.bits.lin_step = 0;
    ssm_cfg1.bits.nun_bin_steps = 8;
    ssm_cfg1.bits.num_lin_steps = 16;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_1, ssm_cfg1.value);
    
    LANE0_RX_ADPTCTL_SSM_SSM_CFG_2_Typedef ssm_cfg2;
    ssm_cfg2.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_2);
    ssm_cfg2.bits.disable_bin_hold = 0; 
    ssm_cfg2.bits.init_bin_step = 7;
    ssm_cfg2.bits.init_dac_code = 256;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_2, ssm_cfg2.value);
    
    LANE0_RX_ADPTCTL_SSM_SSM_CFG_3_Typedef ssm_cfg3;
    ssm_cfg3.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_3);
    ssm_cfg3.bits.ssm_dir = 0;
    ssm_cfg3.bits.sticky_en = 1;
    ssm_cfg3.bits.lpfbyp_en = 0;
    ssm_cfg3.bits.ssm_init_wait = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_3, ssm_cfg3.value);
    
    LANE0_RX_ADPTCTL_SSM_SSM_CFG_4_Typedef ssm_cfg4;
    ssm_cfg4.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_4);
    ssm_cfg4.bits.ssm_n_wait_lpfbyp = 0;
    ssm_cfg4.bits.ssm_n_wait = 40;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_4, ssm_cfg4.value);
    
    DBG("  --> Calibrating 1X Eye:\n");
    match_ctl0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0);
    match_ctl0.bits.pttrn_cr1a_4_0 = 6; 
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0, match_ctl0.value);
    
    // Fix: pttrn_val & 0x2 perfectly translates `bitget(pttrn, 2)` (2^1位)
    if (p->nyquist && !(pttrn_val & 0x2)) {
        e32g_rx_scope_walk_pmix_steps(die, phy, lane, p, &p->current_iqc_code, 1, 40 * (1 << p->rx_rate));
        match_ctl0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0);
        match_ctl0.bits.pttrn_cr1a_4_0 = pttrn_val;
        PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0, match_ctl0.value);
    }
    
    ssm_cfg0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0);
    ssm_cfg0.bits.ssm_dac_sel = scope_addr;
    ssm_cfg0.bits.ssm_th_ofst = 11;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0, ssm_cfg0.value);
    ssm_cfg0.bits.start_ssm = 1; PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0, ssm_cfg0.value);
    ssm_cfg0.bits.start_ssm = 0; PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0, ssm_cfg0.value);
    
    LANE0_RX_ADPTCTL_SSM_FINAL_CODE_Typedef ssm_final;
    int timeout = 10000;
    do { usleep(100); ssm_final.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_FINAL_CODE); } while (!ssm_final.bits.ssm_done && --timeout > 0);
    int top_code = ssm_final.bits.dac_code;
    
    match_ctl0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0);
    match_ctl0.bits.pttrn_cr1a_4_0 = 4;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0, match_ctl0.value);
    
    if (p->nyquist) {
        e32g_rx_scope_walk_pmix_steps(die, phy, lane, p, &p->current_iqc_code, -1, 40 * (1 << p->rx_rate));
        match_ctl0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0);
        match_ctl0.bits.pttrn_cr1a_4_0 = pttrn_val;
        PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0, match_ctl0.value);
    }
    
    ssm_cfg0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0);
    ssm_cfg0.bits.ssm_th_ofst = 3;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0, ssm_cfg0.value);
    ssm_cfg0.bits.start_ssm = 1; PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0, ssm_cfg0.value);
    ssm_cfg0.bits.start_ssm = 0; PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0, ssm_cfg0.value);
    
    timeout = 10000;
    do { usleep(100); ssm_final.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_FINAL_CODE); } while (!ssm_final.bits.ssm_done && --timeout > 0);
    int bot_code = ssm_final.bits.dac_code;
    
    if (p->nyquist && (pttrn_val & 0x2)) {
        e32g_rx_scope_walk_pmix_steps(die, phy, lane, p, &p->current_iqc_code, 1, 40 * (1 << p->rx_rate));
    }
    
    int best_dac_1x = (top_code / 2) + (bot_code / 2);
    DBG("  --> 1X Eye: Top = %d, Bot = %d, Center = %d\n", top_code, bot_code, best_dac_1x);
    
    int best_dac_0x = best_dac_1x;
    if (!p->rx_dfe_byp && !p->nyquist) {
        DBG("  --> Calibrating 0X Eye:\n");
        match_ctl0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0);
        match_ctl0.bits.pttrn_cr1a_4_0 = 2; 
        PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0, match_ctl0.value);
        
        ssm_cfg0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0);
        ssm_cfg0.bits.ssm_th_ofst = 11;
        PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0, ssm_cfg0.value);
        ssm_cfg0.bits.start_ssm = 1; PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0, ssm_cfg0.value);
        ssm_cfg0.bits.start_ssm = 0; PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0, ssm_cfg0.value);
        
        timeout = 10000;
        do { usleep(100); ssm_final.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_FINAL_CODE); } while (!ssm_final.bits.ssm_done && --timeout > 0);
        int top_code_0x = ssm_final.bits.dac_code;
        
        match_ctl0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0);
        match_ctl0.bits.pttrn_cr1a_4_0 = 0;
        PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0, match_ctl0.value);
        
        ssm_cfg0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0);
        ssm_cfg0.bits.ssm_th_ofst = 3;
        PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0, ssm_cfg0.value);
        ssm_cfg0.bits.start_ssm = 1; PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0, ssm_cfg0.value);
        ssm_cfg0.bits.start_ssm = 0; PhyWrite16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_SSM_CFG_0, ssm_cfg0.value);
        
        timeout = 10000;
        do { usleep(100); ssm_final.value = PhyRead16(die, phy, lane, REG_LANE0_RX_ADPTCTL_SSM_FINAL_CODE); } while (!ssm_final.bits.ssm_done && --timeout > 0);
        int bot_code_0x = ssm_final.bits.dac_code;
        
        best_dac_0x = (top_code_0x / 2) + (bot_code_0x / 2);
        DBG("  --> 0X Eye: Top = %d, Bot = %d, Center = %d\n", top_code_0x, bot_code_0x, best_dac_0x);
    }
    
    *code_0x = best_dac_0x;
    DBG("\033[1;32m  --> %s Slicer Calibration Done! 1X Center = %d, 0X Center = %d\033[0m\n", 
           path_type, best_dac_1x, best_dac_0x);
           
    return best_dac_1x;
}

void Matlab_ReadPcsCalibrationCodes(uint32_t die, int phy, int lane, int abs_lane, MarginParams *p) {
    // BUG FIX: 原来 rx_dfe_byp=1 时直接 return，导致 dfe_tap1_code 保持为 0（静态初始化值），
    // 与眼图工具读到的真实 tap1 不一致。实际上 DFE tap1 寄存器在 bypass 模式下仍然保存着
    // firmware 校准的值，应该无条件读取用于显示。
    if (!p->pcs_read) {
        return;
    }

    DBG("\033[1;36m[STEP 0.5] Reading PCS Calibration Codes (Die %u, PHY %d, Lane %d)\033[0m\n", die, phy, lane);

    if (!p->rx_dfe_byp) {
        cal_slc_evn_hi_code[die][abs_lane] = PhyRead16(die, phy, lane, REG_RAWLANEAON0_DFE_DATA0_EVEN_HIGH_VDAC_OFST);
        cal_slc_evn_lo_code[die][abs_lane] = PhyRead16(die, phy, lane, REG_RAWLANEAON0_DFE_DATA0_EVEN_LOW_VDAC_OFST);
        cal_slc_odd_hi_code[die][abs_lane] = PhyRead16(die, phy, lane, REG_RAWLANEAON0_DFE_DATA0_ODD_HIGH_VDAC_OFST);
        cal_slc_odd_lo_code[die][abs_lane] = PhyRead16(die, phy, lane, REG_RAWLANEAON0_DFE_DATA0_ODD_LOW_VDAC_OFST);
    }

    int adapt_sel = PhyRead16(die, phy, lane, REG_RAWLANE0_PCS_XF_RX_PCS_IN_5) & 0x3;

    if (adapt_sel == 1 || adapt_sel == 2) {
        dfe_tap1_code[die][abs_lane] = PhyRead16(die, phy, lane, (adapt_sel == 1) ? REG_RAWLANEAON0_RX_ADPT_DFE_TAP1_B1 : REG_RAWLANEAON0_RX_ADPT_DFE_TAP1_B2);
        if (!p->rx_dfe_byp) {
            cal_slc_evn_hi_tap1_code[die][abs_lane] = PhyRead16(die, phy, lane, (adapt_sel == 1) ? REG_RAWLANEAON0_DFE_DATA1_EVEN_HIGH_TAP1_OFST_B1 : REG_RAWLANEAON0_DFE_DATA1_EVEN_HIGH_TAP1_OFST_B2);
            cal_slc_evn_lo_tap1_code[die][abs_lane] = PhyRead16(die, phy, lane, (adapt_sel == 1) ? REG_RAWLANEAON0_DFE_DATA1_EVEN_LOW_TAP1_OFST_B1 : REG_RAWLANEAON0_DFE_DATA1_EVEN_LOW_TAP1_OFST_B2);
            cal_slc_odd_hi_tap1_code[die][abs_lane] = PhyRead16(die, phy, lane, (adapt_sel == 1) ? REG_RAWLANEAON0_DFE_DATA1_ODD_HIGH_TAP1_OFST_B1 : REG_RAWLANEAON0_DFE_DATA1_ODD_HIGH_TAP1_OFST_B2);
            cal_slc_odd_lo_tap1_code[die][abs_lane] = PhyRead16(die, phy, lane, (adapt_sel == 1) ? REG_RAWLANEAON0_DFE_DATA1_ODD_LOW_TAP1_OFST_B1 : REG_RAWLANEAON0_DFE_DATA1_ODD_LOW_TAP1_OFST_W_VLD_B2);
        }
    } else {
        dfe_tap1_code[die][abs_lane] = PhyRead16(die, phy, lane, REG_RAWLANEAON0_RX_ADPT_DFE_TAP1);
        if (!p->rx_dfe_byp) {
            cal_slc_evn_hi_tap1_code[die][abs_lane] = PhyRead16(die, phy, lane, REG_RAWLANEAON0_DFE_DATA1_EVEN_HIGH_TAP1_OFST);
            cal_slc_evn_lo_tap1_code[die][abs_lane] = PhyRead16(die, phy, lane, REG_RAWLANEAON0_DFE_DATA1_EVEN_LOW_TAP1_OFST);
            cal_slc_odd_hi_tap1_code[die][abs_lane] = PhyRead16(die, phy, lane, REG_RAWLANEAON0_DFE_DATA1_ODD_HIGH_TAP1_OFST);
            cal_slc_odd_lo_tap1_code[die][abs_lane] = PhyRead16(die, phy, lane, REG_RAWLANEAON0_DFE_DATA1_ODD_LOW_TAP1_OFST_W_VLD);
        }
    }

    dfe_tap1_code[die][abs_lane] = dfe_tap1_code[die][abs_lane] & 0x3FFF;
    dfe_tap1_code[die][abs_lane] = (dfe_tap1_code[die][abs_lane] > 8191) ? (dfe_tap1_code[die][abs_lane] - 16384) : dfe_tap1_code[die][abs_lane];
    dfe_tap1_code[die][abs_lane] /= 32;
    if (!p->rx_dfe_byp) {
        cal_slc_evn_hi_tap1_code[die][abs_lane] = (cal_slc_evn_hi_tap1_code[die][abs_lane] > 31) ? (cal_slc_evn_hi_tap1_code[die][abs_lane] - 64) : cal_slc_evn_hi_tap1_code[die][abs_lane];
        cal_slc_evn_lo_tap1_code[die][abs_lane] = (cal_slc_evn_lo_tap1_code[die][abs_lane] > 31) ? (cal_slc_evn_lo_tap1_code[die][abs_lane] - 64) : cal_slc_evn_lo_tap1_code[die][abs_lane];
        cal_slc_odd_hi_tap1_code[die][abs_lane] = (cal_slc_odd_hi_tap1_code[die][abs_lane] > 31) ? (cal_slc_odd_hi_tap1_code[die][abs_lane] - 64) : cal_slc_odd_hi_tap1_code[die][abs_lane];
        cal_slc_odd_lo_tap1_code[die][abs_lane] = (cal_slc_odd_lo_tap1_code[die][abs_lane] > 31) ? (cal_slc_odd_lo_tap1_code[die][abs_lane] - 64) : cal_slc_odd_lo_tap1_code[die][abs_lane];
    }

    DBG("  --> DFE Tap1 Code: %d\n", dfe_tap1_code[die][abs_lane]);
}

void e32g_rx_margin_timing_step(uint32_t die, int phy, int lane, int margin_code, int margin_code_prev, MarginParams* p, int* out_phs, int* out_pmix) {
    int phs_step = 1 << (p->rx_rate - p->phs_step + 1);
    if (phs_step < 1) phs_step = 1;

    int margin_phs_code = phs_code_space[p->current_iqc_code] + phs_step * margin_code;
    margin_phs_code = (margin_phs_code % 70 + 70) % 70;
    int margin_iqc_code = iqc_code_space[margin_phs_code];

    int margin_phs_code_prev = phs_code_space[p->current_iqc_code] + phs_step * margin_code_prev;
    margin_phs_code_prev = (margin_phs_code_prev % 70 + 70) % 70;
    int margin_iqc_code_prev = iqc_code_space[margin_phs_code_prev];

    int iqc_code = margin_iqc_code_prev;
    int incdec = (margin_code > margin_code_prev) ? 1 : ((margin_code < margin_code_prev) ? -1 : 0);
    int num_full_pmix = (phs_step * abs(margin_code - margin_code_prev)) / 70;

    while ((iqc_code != margin_iqc_code || num_full_pmix > 0) && incdec != 0) {
        if (iqc_code == margin_iqc_code) num_full_pmix--;
        iqc_code = (iqc_code + incdec + 80) % 80;
        e32g_rx_scope_walk_pmix_steps(die, phy, lane, p, &p->current_iqc_code, incdec, 1);
    }

    if (out_phs) *out_phs = margin_phs_code;
    if (out_pmix) *out_pmix = pmix_code_space[p->current_iqc_code];
}

void e32g_rx_margin_voltage_step(uint32_t die, int phy, int lane, int margin_code, MarginParams* p, int abs_lane, int* out_evn, int* out_odd, int* overflow) {
    LANE0_ANA_RX_DAC_CTRL_SEL_Typedef dac_sel;
    LANE0_ANA_RX_DAC_CTRL_Typedef dac_ctrl;
    LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN_Typedef dac_en;

    *overflow = 0;

    int margin_val_evn = p->base_dac_evn + margin_code;
    int margin_val_odd = p->base_dac_odd + margin_code;

    if (margin_val_evn > 511 || margin_val_evn < 0 || margin_val_odd > 511 || margin_val_odd < 0) {
        *overflow = 1;
    }
    margin_val_evn = (margin_val_evn > 511) ? 511 : (margin_val_evn < 0 ? 0 : margin_val_evn);
    margin_val_odd = (margin_val_odd > 511) ? 511 : (margin_val_odd < 0 ? 0 : margin_val_odd);

    if (out_evn) *out_evn = margin_val_evn;
    if (out_odd) *out_odd = margin_val_odd;

    dac_sel.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_SEL);
    dac_sel.bits.rx_ana_cal_dac_ctrl_sel = p->scope_evn_addr;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_SEL, dac_sel.value);

    dac_ctrl.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL);
    dac_ctrl.bits.rx_ana_cal_dac_ctrl = margin_val_evn;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL, dac_ctrl.value);

    dac_en.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN);
    dac_en.bits.rx_ana_cal_dac_ctrl_en = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN, dac_en.value);
    usleep(2);
    dac_en.bits.rx_ana_cal_dac_ctrl_en = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN, dac_en.value);

    dac_sel.bits.rx_ana_cal_dac_ctrl_sel = p->scope_odd_addr;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_SEL, dac_sel.value);

    dac_ctrl.bits.rx_ana_cal_dac_ctrl = margin_val_odd;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL, dac_ctrl.value);

    dac_en.bits.rx_ana_cal_dac_ctrl_en = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN, dac_en.value);
    usleep(2);
    dac_en.bits.rx_ana_cal_dac_ctrl_en = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN, dac_en.value);
}

void Matlab_RunNonDestructiveMarginingFull(uint32_t die, int phy, int lane, int abs_lane, int meas_time_ms) {
    MarginParams m_params = {0};
    lane_status[die][abs_lane] = 0;
    m_params.phs_step = 1;
    m_params.dac_step = 1;
    m_params.dac_step_coarse = 4;
    m_params.prev_bit = g_dual_eye ? 0 : 1;
    m_params.meas_errs = 0;
    m_params.meas_errs_coarse = 2;
    m_params.stat_len = 2048;
    m_params.dac_range = 950.0f;
    m_params.debug = 1;
    m_params.nyquist = 0;
    m_params.base_dac_evn_0x = 256;
    m_params.base_dac_odd_0x = 256;
    m_params.pcs_read = 1;

    // 链路状态检查：margin测试要求链路处于L0(active)状态，pstate==0 且 data_en==1。
    // 若链路处于L1/L2或recovery中，scope硬件无法采集有效数据，error counter不递增，
    // 导致sweep走到极限产生虚假极大值(如31.943UI/944.4mV)。
    LANE0_ASIC_RX_ASIC_IN_0_Typedef asic_in_0;
    asic_in_0.value = PhyRead16(die, phy, lane, REG_LANE0_ASIC_RX_ASIC_IN_0);
    if (asic_in_0.value == 0xFFFF) {
        DBG("\033[1;33m[Warning]\033[0m Lane PowerDown or Inactive (ASIC_IN_0=0xFFFF). Skipping.\n");
        lane_status[die][abs_lane] = -3;
        return;
    }

    // 等待链路稳定在L0：前一轮测试的FSM恢复后链路可能经历recovery重训练，
    // 需要等待pstate回到0(P0)且data_en=1才能保证scope采集到有效数据。
    {
        int link_ready = 0;
        for (int wait = 0; wait < 50; wait++) {
            asic_in_0.value = PhyRead16(die, phy, lane, REG_LANE0_ASIC_RX_ASIC_IN_0);
            if (asic_in_0.value == 0xFFFF) break;
            if (asic_in_0.bits.pstate == 0 && asic_in_0.bits.data_en == 1) {
                link_ready = 1;
                break;
            }
            usleep(1000);
        }
        if (!link_ready) {
            DBG("\033[1;33m[Warning]\033[0m Link not in L0 (pstate=%d data_en=%d). Skipping.\n",
                asic_in_0.bits.pstate, asic_in_0.bits.data_en);
            lane_status[die][abs_lane] = -3;
            return;
        }
    }

    m_params.rx_rate = asic_in_0.bits.rate;
    uint16_t rx_dfe_byp = asic_in_0.bits.rx_dfe_bypass;

    LANE0_ASIC_RX_OVRD_IN_0_Typedef ovrd_in_0;
    RAWLANE0_PCS_XF_RX_PACKED_CNTX_RX_RO_OVRD_IN_5_Typedef rx_packen_cntx_ovrd;
    ovrd_in_0.value = PhyRead16(die, phy, lane, REG_LANE0_ASIC_RX_OVRD_IN_0);
    rx_packen_cntx_ovrd.value = PhyRead16(die, phy, lane, REG_RAWLANE0_PCS_XF_RX_PACKED_CNTX_RX_RO_OVRD_IN_5);

    if (ovrd_in_0.bits.rx_dfe_bypass_ovrd_en) {
        rx_dfe_byp = rx_packen_cntx_ovrd.bits.rx_dfe_bypass_ovrd_val;
    }
    m_params.rx_dfe_byp = rx_dfe_byp;

    LANE0_ASIC_RX_OVRD_IN_1_Typedef ovrd_in_1;
    ovrd_in_1.value = PhyRead16(die, phy, lane, REG_LANE0_ASIC_RX_OVRD_IN_1);

    if (ovrd_in_1.bits.rate_ovrd_en) {
        m_params.rx_rate = ovrd_in_1.bits.rate;
    }

    g_rx_rate[die] = m_params.rx_rate;
    g_rx_dfe_byp[die] = m_params.rx_dfe_byp;

    if (!m_params.rx_dfe_byp) {
        meas_time_ms *= 2;
    }

    // BUG FIX: 必须在 FSM override 之前读取 DFE tap1 等 PCS 校准寄存器。
    // FSM override (0x4000) 会冻结 lane 状态机，导致 adaptation 寄存器返回 0 或无效值，
    // 使得 tap1 读出为 0（与眼图工具读到的真实值 14/18/25/10 不一致）。
    if (m_params.pcs_read) {
        Matlab_ReadPcsCalibrationCodes(die, phy, lane, abs_lane, &m_params);
    }

    orig_fsm_val[die][abs_lane] = PhyRead16(die, phy, lane, REG_RAWLANE0_FSM_FSM_OVRD_CTL);
    PhyWrite16(die, phy, lane, REG_RAWLANE0_FSM_FSM_OVRD_CTL, 0x4000);
    usleep(500);

    orig_ana_rx_vco_ovrd_out_1[die][abs_lane] = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_VCO_OVRD_OUT_1);
    orig_ana_rx_ctl_ovrd_out[die][abs_lane] = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_CTL_OVRD_OUT);

    DBG("\n  [Initialization Parameters]\n");
    DBG("RAW ASIC_IN_0: 0x%X\n", asic_in_0.value);
    DBG("  --> RX Rate: %d\n", m_params.rx_rate);
    DBG("  --> DFE Bypass Mode: %s (%d)\n", m_params.rx_dfe_byp ? "Enabled" : "Disabled", m_params.rx_dfe_byp);

    LANE0_RX_STAT_STAT_CTL0_Typedef stat_ctl0_init;
    stat_ctl0_init.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL0);
    if (stat_ctl0_init.value == 0xFFFF || (m_params.rx_rate == 0 && stat_ctl0_init.value == 0x0)) {
        DBG("\033[1;33m[Warning]\033[0m Lane PowerDown or Inactive. Skipping.\n");
        lane_status[die][abs_lane] = -3;
        goto cleanup;
    }
    
    LANE0_ASIC_RX_OVRD_VCO_IN_Typedef ovrd_vco_in;
    ovrd_vco_in.value = PhyRead16(die, phy, lane, REG_LANE0_ASIC_RX_OVRD_VCO_IN);
    uint16_t vco_config;
    
    if (ovrd_vco_in.bits.cdr_vco_config_ovrd_en) {
        vco_config = ovrd_vco_in.bits.cdr_vco_config;
    } else {
        LANE0_ASIC_RX_CDR_VCO_ASIC_IN_Typedef cdr_vco_in;
        cdr_vco_in.value = PhyRead16(die, phy, lane, REG_LANE0_ASIC_RX_CDR_VCO_ASIC_IN);
        vco_config = cdr_vco_in.bits.rx_cdr_vco_config;
    }
    
    LANE0_ANA_RX_VCO_OVRD_OUT_1_Typedef vco_ovrd_out1;
    vco_ovrd_out1.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_VCO_OVRD_OUT_1);
    vco_ovrd_out1.bits.rx_ana_cdr_vco_config = vco_config | (1 << 12);
    vco_ovrd_out1.bits.rx_ana_cdr_vco_config_ovrd_en = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_VCO_OVRD_OUT_1, vco_ovrd_out1.value);
    
    LANE0_ANA_RX_CAL_0_Typedef rx_cal_0;
    rx_cal_0.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_CAL_0);
    rx_cal_0.bits.rx_ana_error_clk_en = 1;
    rx_cal_0.bits.rx_ana_cal_comp_en = 1;
    rx_cal_0.bits.rx_ana_cal_lpfbyp_en_ovrd_en = 1;
    rx_cal_0.bits.rx_ana_cal_lpfbyp_en = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_CAL_0, rx_cal_0.value);
    
    LANE0_ANA_RX_CAL_1_Typedef rx_cal_1;
    rx_cal_1.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_CAL_1);
    rx_cal_1.bits.rx_ana_cal_muxb_sel = 30;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_CAL_1, rx_cal_1.value);
    
    LANE0_ANA_RX_SCOPE_Typedef ana_rx_scope;
    ana_rx_scope.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_SCOPE);
    ana_rx_scope.bits.rx_ana_scope_clk_sel = rx_dfe_byp ? 1 : 0; 
    ana_rx_scope.bits.rx_ana_scope_sel = 1;
    ana_rx_scope.bits.rx_ana_scope_en = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_SCOPE, ana_rx_scope.value);
    usleep(100); 
    
    LANE0_ANA_RX_CTL_OVRD_OUT_Typedef rx_ctl_ovrd;
    rx_ctl_ovrd.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_CTL_OVRD_OUT);
    if (!rx_dfe_byp) {
        rx_ctl_ovrd.bits.rx_ana_bypass_slc_en = 1;
        rx_ctl_ovrd.bits.rx_ana_bypass_slc_en_ovrd_en = 1;
    } else {
        rx_ctl_ovrd.bits.rx_ana_dfe_en = 1;
        rx_ctl_ovrd.bits.rx_ana_dfe_en_ovrd_en = 1;
    }
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_CTL_OVRD_OUT, rx_ctl_ovrd.value);
    
    if (rx_dfe_byp) {
        m_params.scope_evn_addr = 25; 
        m_params.scope_odd_addr = 29;
        m_params.reg_iqc_ovrd = REG_LANE0_ANA_RX_ANA_IQC_DATA_OVRD;           
        m_params.reg_iqc_clk  = REG_LANE0_ANA_RX_ANA_IQC_DATA_ADJUST_CLK;    
    } else {
        m_params.scope_evn_addr = 33; 
        m_params.scope_odd_addr = 34; 
        m_params.reg_iqc_ovrd = REG_LANE0_ANA_RX_ANA_IQC_BYP_OVRD;             
        m_params.reg_iqc_clk  = REG_LANE0_ANA_RX_ANA_IQC_BYPASS_ADJUST_CLK;    
    }
    
    DBG("  --> Assigned Scope EVEN Addr: %d\n", m_params.scope_evn_addr);
    DBG("  --> Assigned Scope ODD Addr:  %d\n", m_params.scope_odd_addr);
    
    int pmix_code = 46;
    if (m_params.rx_rate == 0) pmix_code = 6;
    if (m_params.rx_rate == 1) pmix_code = 112;
    DBG("  --> Init PMIX Code Override: %d (IQC Reg: 0x%X)\n", pmix_code, m_params.reg_iqc_ovrd);
    
    m_params.current_iqc_code = 0; 
    int init_idx = 0;
    for (int i=0; i<80; i++) {
        if (pmix_code_space[i] == pmix_code) {
            init_idx = i;
            break;
        }
    }
    
    LANE0_ANA_RX_ANA_IQC_BYP_OVRD_Typedef iqc_init;
    iqc_init.value = PhyRead16(die, phy, lane, m_params.reg_iqc_ovrd);
    iqc_init.bits.en = 1;
    iqc_init.bits.val = pmix_code;
    PhyWrite16(die, phy, lane, m_params.reg_iqc_ovrd, iqc_init.value);
    
    LANE0_ANA_RX_ANA_IQC_BYPASS_ADJUST_CLK_Typedef iqc_clk_init;
    iqc_clk_init.value = PhyRead16(die, phy, lane, m_params.reg_iqc_clk);
    iqc_clk_init.bits.val = 1;
    PhyWrite16(die, phy, lane, m_params.reg_iqc_clk, iqc_clk_init.value);
    usleep(2);
    iqc_clk_init.bits.val = 0;
    PhyWrite16(die, phy, lane, m_params.reg_iqc_clk, iqc_clk_init.value);
    
    m_params.current_iqc_code = init_idx;
    
    Matlab_InitStatsBlockStatic(die, phy, lane, &m_params);
    int dcc_result = Matlab_ScopeDccCalibrationFull(die, phy, lane, &m_params);

    if (m_params.rx_rate <= 1 && dcc_result == 256) {
        for (int dcc_retry = 0; dcc_retry < 10 && dcc_result == 256; dcc_retry++) {
            usleep(200);
            dcc_result = Matlab_ScopeDccCalibrationFull(die, phy, lane, &m_params);
            DBG("  [DCC Retry %d] result = %d\n", dcc_retry + 1, dcc_result);
        }
    }

    if (m_params.rx_rate <= 1 && dcc_result == 256) {
        int lane_active = 0;
        for (int verify_try = 0; verify_try < 3; verify_try++) {
            Matlab_ConfigStatsCoarse(die, phy, lane);
            int verify_errs = Matlab_RunStatsBlock(die, phy, lane, 2048);
            if (verify_errs >= 0) {
                lane_active = 1;
                break;
            }
            usleep(100);
        }
        if (!lane_active) {
            DBG("  [Warning] DCC not calibrated and lane inactive after 3 checks. Skipping.\n");
            lane_status[die][abs_lane] = -3;
            goto cleanup;
        }
        DBG("  [Warning] DCC returned 256 but lane is active. Continuing with default.\n");
    }
    
    LANE0_ANA_RX_DAC_CTRL_OVRD_Typedef dac_ovrd;
    dac_ovrd.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_OVRD);
    dac_ovrd.bits.rx_cal_dac_ctrl_ovrd = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_OVRD, dac_ovrd.value);
    
    LANE0_ANA_RX_DAC_CTRL_SEL_Typedef dac_sel;
    dac_sel.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_SEL);
    dac_sel.bits.rx_ana_cal_dac_ctrl_sel = m_params.scope_evn_addr;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_SEL, dac_sel.value);
    
    LANE0_ANA_RX_DAC_CTRL_Typedef dac_ctrl;
    dac_ctrl.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL);
    dac_ctrl.bits.rx_ana_cal_dac_ctrl = 256; 
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL, dac_ctrl.value);
    
    LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN_Typedef dac_en;
    dac_en.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN);
    dac_en.bits.rx_ana_cal_dac_ctrl_en = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN, dac_en.value);
    usleep(2);
    dac_en.bits.rx_ana_cal_dac_ctrl_en = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN, dac_en.value);
    
    dac_sel.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_SEL);
    dac_sel.bits.rx_ana_cal_dac_ctrl_sel = m_params.scope_odd_addr;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_SEL, dac_sel.value);
    
    dac_ctrl.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL);
    dac_ctrl.bits.rx_ana_cal_dac_ctrl = 256;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL, dac_ctrl.value);
    
    dac_en.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN);
    dac_en.bits.rx_ana_cal_dac_ctrl_en = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN, dac_en.value);
    usleep(2);
    dac_en.bits.rx_ana_cal_dac_ctrl_en = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN, dac_en.value);
    
    if (m_params.rx_rate <= 1 && m_params.pcs_read) {
        uint16_t pcs_iqc = PhyRead16(die, phy, lane, REG_RAWLANE0_RX_CTL_RX_PHSADJ_LIN) & 0x7F;
        if (pcs_iqc < 80) {
            m_params.current_iqc_code = pcs_iqc;
            LANE0_ANA_RX_ANA_IQC_BYP_OVRD_Typedef iqc_set;
            iqc_set.value = PhyRead16(die, phy, lane, m_params.reg_iqc_ovrd);
            iqc_set.bits.val = pmix_code_space[pcs_iqc];
            iqc_set.bits.en = 1;
            PhyWrite16(die, phy, lane, m_params.reg_iqc_ovrd, iqc_set.value);
            LANE0_ANA_RX_ANA_IQC_BYPASS_ADJUST_CLK_Typedef iqc_clk_set;
            iqc_clk_set.value = PhyRead16(die, phy, lane, m_params.reg_iqc_clk);
            iqc_clk_set.bits.val = 1;
            PhyWrite16(die, phy, lane, m_params.reg_iqc_clk, iqc_clk_set.value);
            usleep(2);
            iqc_clk_set.bits.val = 0;
            PhyWrite16(die, phy, lane, m_params.reg_iqc_clk, iqc_clk_set.value);
            DBG("  --> Using PCS IQC code: %d (PMIX=%d)\n", pcs_iqc, pmix_code_space[pcs_iqc]);
        }
    }

    m_params.current_iqc_code = Matlab_ScopeBitAlignmentFull(die, phy, lane, &m_params);
    if (m_params.current_iqc_code == -1) { lane_status[die][abs_lane] = -1; goto cleanup; }

    int pttrn_evn, pttrn_odd;
    Matlab_PatternCheck(die, phy, lane, &m_params, &pttrn_evn, &pttrn_odd);
    int pttrn_evn_val = (pttrn_evn == 0) ? 0 : __builtin_ctz(pttrn_evn);
    int pttrn_odd_val = (pttrn_odd == 0) ? 0 : __builtin_ctz(pttrn_odd);

    dac_ovrd.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_OVRD);
    dac_ovrd.bits.rx_cal_dac_ctrl_ovrd = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_OVRD, dac_ovrd.value);

    m_params.base_dac_evn = Matlab_ScopeSlicerCalibrationFull(die, phy, lane, m_params.scope_evn_addr, 0xAAAA, &m_params, 1, &m_params.base_dac_evn_0x, pttrn_evn_val);
    m_params.base_dac_odd = Matlab_ScopeSlicerCalibrationFull(die, phy, lane, m_params.scope_odd_addr, 0x5555, &m_params, 0, &m_params.base_dac_odd_0x, pttrn_odd_val);

    // 使用PCS firmware校准的DAC值替代scope slicer校准值作为voltage sweep基准。
    // 参考xGMI破坏性margin实现(prbs_loopback.c get_base_voltage_dac)：
    // PCS firmware在链路训练时已精确校准每个slicer的DAC中心点，
    // 而scope slicer calibration的(top+bot)/2只是scope硬件的近似中心，
    // 与真实数据眼中心存在偏差，导致voltage sweep上下严重不对称。
    if (m_params.pcs_read) {
        if (m_params.rx_dfe_byp) {
            int pcs_evn = PhyRead16(die, phy, lane, REG_RAWLANEAON0_DFE_BYPASS_EVEN_VDAC_OFST) & 0x1FF;
            int pcs_odd = PhyRead16(die, phy, lane, REG_RAWLANEAON0_DFE_BYPASS_ODD_VDAC_OFST) & 0x1FF;
            if (pcs_evn > 0 && pcs_evn < 511 && pcs_odd > 0 && pcs_odd < 511) {
                m_params.base_dac_evn = pcs_evn;
                m_params.base_dac_odd = pcs_odd;
                DBG("  --> Using PCS DAC (bypass): evn=%d odd=%d\n", pcs_evn, pcs_odd);
            }
        } else {
            int pcs_evn_hi = PhyRead16(die, phy, lane, REG_RAWLANEAON0_DFE_DATA0_EVEN_HIGH_VDAC_OFST) & 0x1FF;
            int pcs_evn_lo = PhyRead16(die, phy, lane, REG_RAWLANEAON0_DFE_DATA0_EVEN_LOW_VDAC_OFST) & 0x1FF;
            int pcs_odd_hi = PhyRead16(die, phy, lane, REG_RAWLANEAON0_DFE_DATA0_ODD_HIGH_VDAC_OFST) & 0x1FF;
            int pcs_odd_lo = PhyRead16(die, phy, lane, REG_RAWLANEAON0_DFE_DATA0_ODD_LOW_VDAC_OFST) & 0x1FF;
            int pcs_evn_avg = (pcs_evn_hi + pcs_evn_lo) / 2;
            int pcs_odd_avg = (pcs_odd_hi + pcs_odd_lo) / 2;
            if (pcs_evn_avg > 0 && pcs_evn_avg < 511 && pcs_odd_avg > 0 && pcs_odd_avg < 511) {
                m_params.base_dac_evn = pcs_evn_avg;
                m_params.base_dac_odd = pcs_odd_avg;
                DBG("  --> Using PCS DAC (DFE): evn_hi=%d evn_lo=%d odd_hi=%d odd_lo=%d => evn=%d odd=%d\n",
                    pcs_evn_hi, pcs_evn_lo, pcs_odd_hi, pcs_odd_lo, pcs_evn_avg, pcs_odd_avg);
            }
        }
    }

    if (m_params.rx_rate <= 1 && m_params.pcs_read) {
        uint16_t pcs_iqc_final = PhyRead16(die, phy, lane, REG_RAWLANE0_RX_CTL_RX_PHSADJ_LIN) & 0x7F;
        if (pcs_iqc_final < 80) {
            int old_iqc = m_params.current_iqc_code;
            int incdec = (m_params.current_iqc_code > (int)pcs_iqc_final) ? -1 : 1;
            int steps = abs(m_params.current_iqc_code - (int)pcs_iqc_final);
            if (steps > 40) { incdec = -incdec; steps = 80 - steps; }
            e32g_rx_scope_walk_pmix_steps(die, phy, lane, &m_params, &m_params.current_iqc_code, incdec, steps);
            DBG("  --> Timing center: restored PCS IQC %d (was %d from bit-align)\n", pcs_iqc_final, old_iqc);
        }
    }

    DBG("\n  [Action] Applying Optimal Configurations Found:\n");
    DBG("  --> PMIX Code:  %d\n", pmix_code_space[m_params.current_iqc_code]);
    DBG("  --> Evn DAC (1X): %d, (0X): %d\n", m_params.base_dac_evn, m_params.base_dac_evn_0x);
    DBG("  --> Odd DAC (1X): %d, (0X): %d\n", m_params.base_dac_odd, m_params.base_dac_odd_0x);
    
    LANE0_ANA_RX_ANA_IQC_BYP_OVRD_Typedef iqc_ovrd;
    iqc_ovrd.value = PhyRead16(die, phy, lane, m_params.reg_iqc_ovrd);
    iqc_ovrd.bits.val = pmix_code_space[m_params.current_iqc_code];
    iqc_ovrd.bits.en = 1;
    PhyWrite16(die, phy, lane, m_params.reg_iqc_ovrd, iqc_ovrd.value);
    
    LANE0_ANA_RX_ANA_IQC_BYPASS_ADJUST_CLK_Typedef iqc_clk;
    iqc_clk.value = PhyRead16(die, phy, lane, m_params.reg_iqc_clk);
    iqc_clk.bits.val = 1;
    PhyWrite16(die, phy, lane, m_params.reg_iqc_clk, iqc_clk.value);
    usleep(2);
    iqc_clk.bits.val = 0;
    PhyWrite16(die, phy, lane, m_params.reg_iqc_clk, iqc_clk.value);
    
    dac_ovrd.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_OVRD);
    dac_ovrd.bits.rx_cal_dac_ctrl_ovrd = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_OVRD, dac_ovrd.value);
    
    dac_sel.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_SEL);
    dac_sel.bits.rx_ana_cal_dac_ctrl_sel = m_params.scope_evn_addr;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_SEL, dac_sel.value);
    
    dac_ctrl.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL);
    dac_ctrl.bits.rx_ana_cal_dac_ctrl = m_params.base_dac_evn;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL, dac_ctrl.value);
    
    dac_en.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN);
    dac_en.bits.rx_ana_cal_dac_ctrl_en = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN, dac_en.value);
    usleep(2);
    dac_en.bits.rx_ana_cal_dac_ctrl_en = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN, dac_en.value);
    
    dac_sel.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_SEL);
    dac_sel.bits.rx_ana_cal_dac_ctrl_sel = m_params.scope_odd_addr;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_SEL, dac_sel.value);
    
    dac_ctrl.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL);
    dac_ctrl.bits.rx_ana_cal_dac_ctrl = m_params.base_dac_odd;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL, dac_ctrl.value);
    
    dac_en.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN);
    dac_en.bits.rx_ana_cal_dac_ctrl_en = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN, dac_en.value);
    usleep(2);
    dac_en.bits.rx_ana_cal_dac_ctrl_en = 0;
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN, dac_en.value);
    
    uint32_t pttrn_msk = m_params.rx_dfe_byp ? 0 : 4;
    uint32_t pttrn = (m_params.prev_bit == 1) ? 4 : 0;

    if (m_params.prev_bit == 0) {
        m_params.base_dac_evn = m_params.base_dac_evn_0x;
        m_params.base_dac_odd = m_params.base_dac_odd_0x;
    }

    {
        LANE0_ANA_RX_DAC_CTRL_SEL_Typedef ds;
        LANE0_ANA_RX_DAC_CTRL_Typedef dc;
        LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN_Typedef de;
        ds.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_SEL);
        ds.bits.rx_ana_cal_dac_ctrl_sel = m_params.scope_evn_addr;
        PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_SEL, ds.value);
        dc.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL);
        dc.bits.rx_ana_cal_dac_ctrl = m_params.base_dac_evn;
        PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL, dc.value);
        de.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN);
        de.bits.rx_ana_cal_dac_ctrl_en = 1;
        PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN, de.value);
            usleep(2);
            de.bits.rx_ana_cal_dac_ctrl_en = 0;
            PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN, de.value);

            ds.bits.rx_ana_cal_dac_ctrl_sel = m_params.scope_odd_addr;
            PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_SEL, ds.value);
            dc.bits.rx_ana_cal_dac_ctrl = m_params.base_dac_odd;
            PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL, dc.value);
            de.bits.rx_ana_cal_dac_ctrl_en = 1;
            PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN, de.value);
            usleep(2);
            de.bits.rx_ana_cal_dac_ctrl_en = 0;
            PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_ANA_CAL_DAC_CTRL_EN, de.value);
        }

        PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_DATA_MSK, 0xFFFF);

    LANE0_RX_STAT_MATCH_CTL0_Typedef match_ctl0;
    match_ctl0.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0);
    match_ctl0.bits.data_msk_19_16 = 0xF;
    match_ctl0.bits.pttrn_cr1a_4_0 = pttrn & 0x1F;
    match_ctl0.bits.pttrn_msk_cr1a_4_0 = pttrn_msk & 0x1F;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_MATCH_CTL0, match_ctl0.value);

    LANE0_RX_STAT_STAT_CTL0_Typedef ctl0_final;
    ctl0_final.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL0);
    ctl0_final.bits.sc_timer_mode = 1;
    ctl0_final.bits.stat_src_sel = 4;
    ctl0_final.bits.corr_mode_en = 1;
    ctl0_final.bits.corr_src_sel = 3;
    ctl0_final.bits.stat_rxclk_sel = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL0, ctl0_final.value);

    LANE0_RX_STAT_STAT_CTL2_Typedef ctl2_final;
    ctl2_final.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL2);
    ctl2_final.bits.disable_sample_count = 1;
    PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_STAT_CTL2, ctl2_final.value);

    DBG("\n  [Info] Running Center Point Error Check (Len = 32000)\n");
    Matlab_ConfigStatsCoarse(die, phy, lane);
    int center_errs = Matlab_RunStatsBlock(die, phy, lane, 32000);

    if (center_errs == -2 || center_errs == -1) {
        DBG("\033[1;31m[FSM Deadlock]\033[0m Timeout or Disconnected!\n");
        lane_status[die][abs_lane] = -1;
        goto cleanup;
    } else if (center_errs > 100) {
        DBG("\033[1;31m[Center Error]\033[0m errs=%d (Eye closed or not aligned)\n", center_errs);
        lane_status[die][abs_lane] = -2;
        goto cleanup;
    } else {
        DBG("\033[1;32m[Aligned Success]\033[0m Center OK, errors=%d\n", center_errs);
    }
    
    DBG("\033[1;36m[STEP 4] Phase(Timing) and Voltage Sweeping (Die %u, PHY %d, Lane %d)\033[0m\n", die, phy, lane);
    DBG("  [Test Config] BER Target: meas_errs=%d within %.3f sec\n", m_params.meas_errs, meas_time_ms / 1000.0f);
    DBG("  [Test Config] Coarse: %d samples/step, Fine: %.3fs continuous monitoring\n", m_params.stat_len, meas_time_ms / 1000.0f);
    DBG("  [Test Config] Timing step: 1/%d UI, Voltage step: %.2f mV (coarse: %.2f mV)\n",
           (int)(((1 << m_params.rx_rate) / (float)(1 << (m_params.rx_rate - m_params.phs_step + 1))) * 35),
           m_params.dac_range / 511.0f * m_params.dac_step,
           m_params.dac_range / 511.0f * m_params.dac_step_coarse);
    
    int left = 0, right = 0, up = 0, down = 0;
    int cur_phs_val = 0, cur_pmix_val = 0;
    int cur_evn_dac = 0, cur_odd_dac = 0;
    int overflow = 0;

    Matlab_ConfigStatsCoarse(die, phy, lane);
    DBG("  [Timing Sweep RIGHT - Coarse]\n  ");
    int coarse_right = 560;
    for(int step = 1; step <= 560; step++) {
        e32g_rx_margin_timing_step(die, phy, lane, step, step - 1, &m_params, &cur_phs_val, &cur_pmix_val);
        if (Matlab_MarginErrorCheckCoarse(die, phy, lane, m_params.meas_errs_coarse)) { coarse_right = step; break; }
        if (step % 10 == 9) DBG("[S:%2d Phs:%3d] ", step, cur_phs_val);
    }
    // Coarse sweep reaches limit without errors => link not in L0 or eye is impossibly wide.
    // Early abort: skip fine sweep (560 iterations × meas_time_ms would take ~52s for nothing).
    if (coarse_right == 560) {
        DBG("\033[1;33m  [Coarse Limit Reached]\033[0m No errors in 560 steps. Skipping fine sweep.\n");
        left = right = 0; up = down = 0;
        eye_width_ui[die][abs_lane] = 0.0f;
        safe_iq_min_ui[die][abs_lane] = 0.0f;
        safe_iq_max_ui[die][abs_lane] = 0.0f;
        eye_height_mv[die][abs_lane] = 0.0f;
        safe_vdac_min_mv[die][abs_lane] = 0.0f;
        safe_vdac_max_mv[die][abs_lane] = 0.0f;
        lane_status[die][abs_lane] = -3;
        goto cleanup;
    }
    Matlab_ConfigStatsFine(die, phy, lane);
    DBG("\n  [Timing Sweep RIGHT - Fine]\n  ");
    for(int step = coarse_right - 1; step >= 0; step--) {
        e32g_rx_margin_timing_step(die, phy, lane, step, step + 1, &m_params, &cur_phs_val, &cur_pmix_val);
        if (!Matlab_MarginErrorCheck(die, phy, lane, meas_time_ms, m_params.meas_errs)) { right = step; break; }
    }
    for(int step = right - 1; step >= 0; step--) {
        e32g_rx_margin_timing_step(die, phy, lane, step, step + 1, &m_params, NULL, NULL);
    }

    Matlab_ConfigStatsCoarse(die, phy, lane);
    DBG("\n  [Timing Sweep LEFT - Coarse]\n  ");
    int coarse_left = 560;
    for(int step = 1; step <= 560; step++) {
        e32g_rx_margin_timing_step(die, phy, lane, -step, -(step - 1), &m_params, &cur_phs_val, &cur_pmix_val);
        if (Matlab_MarginErrorCheckCoarse(die, phy, lane, m_params.meas_errs_coarse)) { coarse_left = step; break; }
        if (step % 10 == 9) DBG("[S:%3d Phs:%3d] ", -step, cur_phs_val);
    }
    Matlab_ConfigStatsFine(die, phy, lane);
    DBG("\n  [Timing Sweep LEFT - Fine]\n  ");
    for(int step = coarse_left - 1; step >= 0; step--) {
        e32g_rx_margin_timing_step(die, phy, lane, -step, -(step + 1), &m_params, &cur_phs_val, &cur_pmix_val);
        if (!Matlab_MarginErrorCheck(die, phy, lane, meas_time_ms, m_params.meas_errs)) { left = step; break; }
    }
    for(int step = left - 1; step >= 0; step--) {
        e32g_rx_margin_timing_step(die, phy, lane, -step, -(step + 1), &m_params, NULL, NULL);
    }

    Matlab_ConfigStatsCoarse(die, phy, lane);
    DBG("\n  [Voltage Sweep UP - Coarse]\n  ");
    int coarse_up = 512;
    for(int step = m_params.dac_step_coarse; step <= 512; step += m_params.dac_step_coarse) {
        e32g_rx_margin_voltage_step(die, phy, lane, step, &m_params, abs_lane, &cur_evn_dac, &cur_odd_dac, &overflow);
        if (overflow || Matlab_MarginErrorCheckCoarse(die, phy, lane, m_params.meas_errs_coarse)) { coarse_up = step; break; }
        if (step % (m_params.dac_step_coarse * 4) == 0) DBG("[S:%2d VDAC:%3d] ", step, cur_evn_dac);
    }
    Matlab_ConfigStatsFine(die, phy, lane);
    DBG("\n  [Voltage Sweep UP - Fine]\n  ");
    for(int step = coarse_up - m_params.dac_step_coarse; step >= 0; step -= m_params.dac_step) {
        e32g_rx_margin_voltage_step(die, phy, lane, step, &m_params, abs_lane, &cur_evn_dac, &cur_odd_dac, &overflow);
        if (!overflow && !Matlab_MarginErrorCheck(die, phy, lane, meas_time_ms, m_params.meas_errs)) { up = step; break; }
    }
    e32g_rx_margin_voltage_step(die, phy, lane, 0, &m_params, abs_lane, NULL, NULL, &overflow);

    Matlab_ConfigStatsCoarse(die, phy, lane);
    DBG("\n  [Voltage Sweep DOWN - Coarse]\n  ");
    int coarse_down = 512;
    for(int step = m_params.dac_step_coarse; step <= 512; step += m_params.dac_step_coarse) {
        e32g_rx_margin_voltage_step(die, phy, lane, -step, &m_params, abs_lane, &cur_evn_dac, &cur_odd_dac, &overflow);
        if (overflow || Matlab_MarginErrorCheckCoarse(die, phy, lane, m_params.meas_errs_coarse)) { coarse_down = step; break; }
        if (step % (m_params.dac_step_coarse * 4) == 0) DBG("[S:%3d VDAC:%3d] ", -step, cur_evn_dac);
    }
    Matlab_ConfigStatsFine(die, phy, lane);
    DBG("\n  [Voltage Sweep DOWN - Fine]\n  ");
    for(int step = coarse_down - m_params.dac_step_coarse; step >= 0; step -= m_params.dac_step) {
        e32g_rx_margin_voltage_step(die, phy, lane, -step, &m_params, abs_lane, &cur_evn_dac, &cur_odd_dac, &overflow);
        if (!overflow && !Matlab_MarginErrorCheck(die, phy, lane, meas_time_ms, m_params.meas_errs)) { down = step; break; }
    }

    e32g_rx_margin_voltage_step(die, phy, lane, 0, &m_params, abs_lane, NULL, NULL, &overflow);

    timing_left[die][abs_lane]  = -left;
    timing_right[die][abs_lane] = right;
    voltage_down[die][abs_lane] = -down;
    voltage_up[die][abs_lane]   = up;

    int actual_phs_step = 1 << (m_params.rx_rate - m_params.phs_step + 1);
    if (actual_phs_step < 1) actual_phs_step = 1;
    float phs_step_ui = ((1 << m_params.rx_rate) / (float)actual_phs_step) * 35.0f;
    eye_width_ui[die][abs_lane]   = (float)(right + left) / phs_step_ui;
    safe_iq_min_ui[die][abs_lane] = (float)(-left) / phs_step_ui;
    safe_iq_max_ui[die][abs_lane] = (float)right / phs_step_ui;

    float dac_step_mv = (m_params.dac_range / 511.0f);
    eye_height_mv[die][abs_lane]    = (float)(up + down) * dac_step_mv;
    safe_vdac_min_mv[die][abs_lane] = (float)(-down) * dac_step_mv;
    safe_vdac_max_mv[die][abs_lane] = (float)up * dac_step_mv;

    // 结果合理性校验：Gen5物理极限约为0.6UI宽、300mV高。
    // 若timing coarse sweep走到极限(560)未找到error边界，说明scope未采集到有效数据，
    // 根因是链路在测试过程中离开了L0(如进入recovery)，error counter不递增。
    // 此时结果无意义，标记为SKIP避免输出误导性极大值。
    if (eye_width_ui[die][abs_lane] > 1.0f || eye_height_mv[die][abs_lane] > 500.0f) {
        DBG("\033[1;33m[Sanity Fail]\033[0m Results exceed physical limits (UI=%.3f mV=%.1f). "
            "Link likely left L0 during test.\n",
            eye_width_ui[die][abs_lane], eye_height_mv[die][abs_lane]);
        lane_status[die][abs_lane] = -3;
    }

cleanup:
    DBG("\n  [Cleanup] Restoring original register values...\n");

    {
        LANE0_ANA_RX_ANA_IQC_BYP_OVRD_Typedef iqc_ovrd_cleanup;
        iqc_ovrd_cleanup.value = PhyRead16(die, phy, lane, m_params.reg_iqc_ovrd);
        iqc_ovrd_cleanup.bits.en = 0;
        PhyWrite16(die, phy, lane, m_params.reg_iqc_ovrd, iqc_ovrd_cleanup.value);
    }

    {
        LANE0_ANA_RX_DAC_CTRL_OVRD_Typedef dac_ovrd_cleanup;
        dac_ovrd_cleanup.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_OVRD);
        dac_ovrd_cleanup.bits.rx_cal_dac_ctrl_ovrd = 0;
        PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_DAC_CTRL_OVRD, dac_ovrd_cleanup.value);
    }

    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_VCO_OVRD_OUT_1, orig_ana_rx_vco_ovrd_out_1[die][abs_lane]);
    PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_CTL_OVRD_OUT, orig_ana_rx_ctl_ovrd_out[die][abs_lane]);

    {
        LANE0_ANA_RX_SCOPE_Typedef scope_cleanup;
        scope_cleanup.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_SCOPE);
        scope_cleanup.bits.rx_ana_scope_en = 0;
        PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_SCOPE, scope_cleanup.value);
    }

    {
        LANE0_ANA_RX_CAL_0_Typedef cal0_cleanup;
        cal0_cleanup.value = PhyRead16(die, phy, lane, REG_LANE0_ANA_RX_CAL_0);
        cal0_cleanup.bits.rx_ana_error_clk_en = 0;
        PhyWrite16(die, phy, lane, REG_LANE0_ANA_RX_CAL_0, cal0_cleanup.value);
    }

    {
        LANE0_RX_STAT_STAT_STOP_Typedef stop_ctl;
        stop_ctl.value = PhyRead16(die, phy, lane, REG_LANE0_RX_STAT_STAT_STOP);
        stop_ctl.bits.sc1_stop = 1;
        PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_STAT_STOP, stop_ctl.value);
        usleep(5);
        stop_ctl.bits.sc1_stop = 0;
        PhyWrite16(die, phy, lane, REG_LANE0_RX_STAT_STAT_STOP, stop_ctl.value);
    }

    PhyWrite16(die, phy, lane, REG_RAWLANE0_FSM_FSM_OVRD_CTL, orig_fsm_val[die][abs_lane]);

    // FSM恢复后等待链路重新训练回L0：释放FSM override后PHY会重新进入正常工作流程，
    // 链路可能经历短暂的recovery。等待pstate回到0确保下一个lane测试时链路已稳定。
    // 若不等待，连续测试多lane时后续lane可能在链路不稳定时开始，导致DCC校准失败
    // (需要多次retry增加耗时)或scope采集无效数据(产生极大值)。
    {
        for (int settle = 0; settle < 20; settle++) {
            usleep(1000);
            LANE0_ASIC_RX_ASIC_IN_0_Typedef settle_chk;
            settle_chk.value = PhyRead16(die, phy, lane, REG_LANE0_ASIC_RX_ASIC_IN_0);
            if (settle_chk.value != 0xFFFF && settle_chk.bits.pstate == 0 && settle_chk.bits.data_en == 1)
                break;
        }
    }

    DBG("\033[1;32m  --> Cleanup complete. Lane %d test finished.\033[0m\n", abs_lane);
}


void PrintAllResults(int die_start, int die_end, int total_lanes, float meas_time, int ber_depth) {
    // const char *speed_str[] = {"32GT/s(Gen5)", "16GT/s(Gen4)", "8GT/s(Gen3)", "5GT/s(Gen2)", "2.5GT/s(Gen1)"};

    // printf("\n  Link Speed per Die: ");
    // for (int d = die_start; d < die_end; d++) {
    //     int idx = g_rx_rate[d];
    //     if (idx < 0) idx = 0;
    //     if (idx > 4) idx = 4;
    //     printf("Die%d=%s  ", d, speed_str[idx]);
    // }
    // printf("\n\n");

    // printf("---------+--------------+-------+--------+-----------------+-------+-----------------+---------\n");
    // printf("  D_P_L  | BDF          |  DFE1 |   UI   |  Left    Right  |   mV  |   Down     Up   | BER<1e-%d\n", ber_depth);
    // printf("---------+--------------+-------+--------+-----------------+-------+-----------------+---------\n");

    printf("---------+--------------+--------+-----------------+-------+-----------------\n");
    printf("  D_P_L  | BDF          |   UI   |  Left    Right  |   mV  |   Down     Up\n");
    printf("---------+--------------+--------+-----------------+-------+-----------------\n");

    for (int d = die_start; d < die_end; d++) {
        for (int l = 0; l < total_lanes; l++) {
            int phy_id = l / MAX_LANES;
            char *bdf = GetHcuBDFStr(d);

            // BUG FIX: 原来 lane_status==-3 时直接 continue 不打印任何信息，
            // 导致用户看不到哪些 lane 被跳过，误以为测试结果丢失。
            // 现在统一打印 SKIP 行，让用户明确知道该 lane 未参与测试。
            if (lane_status[d][l] == -3) {
                printf(" %02d_%d_%02d | %4s |  ---   |  ---     ---   |  ---  |  ---      ---   | SKIP\n",
                       d, phy_id, l, bdf ? bdf : "??");
                continue;
            }

            // EYE_CLS(-2): center error check失败，眼图闭合。输出全0保持格式一致。
            // TIMEOUT(-1)/SKIP(-3): 输出SKIP行。
            if (lane_status[d][l] == -2) {
                printf(" %02d_%d_%02d | %4s | 0.000  | +0.00    +0.00  |   0.0 |   +0.0    +0.0\n",
                       d, phy_id, l, bdf ? bdf : "??");
                continue;
            }
            if (lane_status[d][l] < 0) {
                printf(" %02d_%d_%02d | %4s |  ---   |  ---     ---   |  ---  |  ---      ---   | SKIP\n",
                       d, phy_id, l, bdf ? bdf : "??");
                continue;
            }

            float w = eye_width_ui[d][l];
            float h = eye_height_mv[d][l];
            // int pass = (w > 0.3f && h > 15.0f);

            // printf(" %02d_%d_%02d | %4s | %+5d | %5.3f  | %+5.2f    %+5.2f  | %5.1f | %+6.1f  %+6.1f  | %s\n",
            printf(" %02d_%d_%02d | %4s | %5.3f  | %+5.2f    %+5.2f  | %5.1f | %+6.1f  %+6.1f\n",
                   d, phy_id, l, bdf ? bdf : "??",
                   w, safe_iq_min_ui[d][l], safe_iq_max_ui[d][l],
                   h, safe_vdac_min_mv[d][l], safe_vdac_max_mv[d][l]
                //    , pass ? "\033[1;32m PASS \033[0m" : "\033[1;31m FAIL \033[0m"
                   );
        }
    }
    // printf("---------+--------------+-------+--------+-----------------+-------+-----------------+---------\n");
    printf("---------+--------------+--------+-----------------+-------+-----------------\n");
    printf("  BER Target: 0 errors in %.3fs/step (DFE: %.3fs) | DAC: 950mV/511 | Phase: 35/UI\n\n", meas_time, meas_time * 2);
}

void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("  -d <die>     Test specific die (0-based). Default: all dies\n");
    printf("  -l <lane>    Test specific lane (0-15). Default: all lanes\n");
    printf("  -b <depth>   BER depth: 10/11/12 for 1e-10/1e-11/1e-12. Default: 10\n");
    printf("  -v           Enable verbose debug output. Default: off\n");
    printf("  -p           Enable parallel testing across dies. Default: sequential\n");
    printf("  -2           Measure 0X eye (prev_bit=0). Default: 1X eye (prev_bit=1)\n");
    printf("  -h           Show this help\n");
    printf("\nExamples:\n");
    printf("  %s                  Test all dies, all lanes, BER 1e-12\n", prog);
    printf("  %s -d 0 -b 10      Test die 0, all lanes, BER 1e-10 (fast)\n", prog);
    printf("  %s -d 1 -l 3 -v    Test die 1, lane 3, verbose\n", prog);
    printf("  %s -b 10 -p        All dies parallel, BER 1e-10\n", prog);
}

typedef struct {
    int die_id;
    int target_lane;
    int meas_time_ms;
} DieThreadArg;

void* die_test_thread(void *arg) {
    DieThreadArg *ta = (DieThreadArg *)arg;
    int die = ta->die_id;
    for (int phy = 0; phy < MAX_PHYS; phy++) {
        for (int lane = 0; lane < MAX_LANES; lane++) {
            int abs_lane = phy * MAX_LANES + lane;
            if (ta->target_lane >= 0 && abs_lane != ta->target_lane) {
                lane_status[die][abs_lane] = -3;
                continue;
            }
            Matlab_RunNonDestructiveMarginingFull(die, phy, lane, abs_lane, ta->meas_time_ms);
        }
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    int target_die = -1;
    int target_lane = -1;
    int ber_depth = 10;
    int parallel = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            target_die = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            target_lane = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            ber_depth = atoi(argv[++i]);
            if (ber_depth < 10 || ber_depth > 12) {
                printf("Error: BER depth must be 10, 11, or 12\n");
                return -1;
            }
        } else if (strcmp(argv[i], "-v") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "-p") == 0) {
            parallel = 1;
        } else if (strcmp(argv[i], "-2") == 0) {
            g_dual_eye = 1;
        } else if (strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            printf("Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return -1;
        }
    }

    float meas_time_sec;
    if (ber_depth == 12) meas_time_sec = 9.375f;
    else if (ber_depth == 11) meas_time_sec = 0.9375f;
    else meas_time_sec = 0.09375f;
    int meas_time_ms = (int)(meas_time_sec * 1000);

    if (SmnInitAll() != 0) { return -1; }
    Matlab_InitCodeSpaces();

    int num_dies = SmnGetDieNum();
    int die_start = 0, die_end = num_dies;
    if (target_die >= 0) {
        if (target_die >= num_dies) {
            printf("Error: die %d not found (system has %d dies)\n", target_die, num_dies);
            SmnDestroy();
            return -1;
        }
        die_start = target_die;
        die_end = target_die + 1;
    }

    printf("pMargin v%s | Build: %s | Commit: %s\n", PMARGIN_VERSION, BUILD_TIME, GIT_INFO);
    // printf("=== Non-Destructive RX Margin Test ===\n");
    // printf("  Dies: %d | BER: 1e-%d (%.3fs/step) | Parallel: %s | Eye: %s | Verbose: %s\n\n",
    //        num_dies, ber_depth, meas_time_sec, parallel ? "ON" : "OFF", g_dual_eye ? "0X" : "1X", g_verbose ? "ON" : "OFF");

    int total_lanes = MAX_PHYS * MAX_LANES;
    memset(lane_status, 0, sizeof(lane_status));

    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    if (parallel && (die_end - die_start) > 1) {
        pthread_t threads[MAX_DIES];
        DieThreadArg args[MAX_DIES];
        int num_threads = 0;

        for (int die = die_start; die < die_end; die++) {
            args[num_threads].die_id = die;
            args[num_threads].target_lane = target_lane;
            args[num_threads].meas_time_ms = meas_time_ms;
            pthread_create(&threads[num_threads], NULL, die_test_thread, &args[num_threads]);
            num_threads++;
        }
        for (int i = 0; i < num_threads; i++) {
            pthread_join(threads[i], NULL);
        }
    } else {
        for (int die = die_start; die < die_end; die++) {
            if (!g_verbose) printf("  [Die %d] Testing...", die);
            for (int phy = 0; phy < MAX_PHYS; phy++) {
                for (int lane = 0; lane < MAX_LANES; lane++) {
                    int abs_lane = phy * MAX_LANES + lane;
                    if (target_lane >= 0 && abs_lane != target_lane) {
                        lane_status[die][abs_lane] = -3;
                        continue;
                    }
                    Matlab_RunNonDestructiveMarginingFull(die, phy, lane, abs_lane, meas_time_ms);
                }
            }
            if (!g_verbose) printf(" Done.\n");
        }
    }

    PrintAllResults(die_start, die_end, total_lanes, meas_time_sec, ber_depth);

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed = (ts_end.tv_sec - ts_start.tv_sec) + (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;
    int mins = (int)(elapsed / 60);
    double secs = elapsed - mins * 60;
    printf("  Total test time: %dm %.1fs (%.1f seconds)\n\n", mins, secs, elapsed);

    SmnDestroy();
    return 0;
}
