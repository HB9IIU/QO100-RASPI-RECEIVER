#pragma once

/* Screen-size profiles: every layout measurement that differs between the
 * screen sizes the app is designed for, one table row per size.
 *
 * The layout code itself is written once, for all sizes. Where a size needs
 * its own value - a font size, a card height, a button width - the drawing
 * and hit-testing code reads it from the active profile rather than choosing
 * between hard-coded numbers on the spot. Proportional things (the main
 * page's big panels, a card filling what is left) stay as formulas and are
 * not listed here.
 *
 * Adding a screen size means adding a row to kUiProfiles, and checking every
 * page at that size with the screenshot mode (QO100_SCREENSHOT_PAGE, see
 * main()). The two original rows must keep their values: those screens are
 * in use, and a change there shows up in the before/after screenshots.
 *
 * Text is drawn at any font size (TextCache loads sizes on first use), so
 * the font sizes below can be anything. */

#include <cstddef>

namespace qo100 {

struct UiProfile {
    const char * name;      /* "1024x600" - also the SET page's label */
    int width;              /* the screen size this row is designed for */
    int height;

    /* ---- main page ---- */
    struct Main {
        /* Space below the spectrum plot, inside its panel. The frequency
         * axis labels (10491 ... 10498) are drawn there only when
         * spectrum_axis_labels is set; without them the strip is small and
         * the plot gets the rest (see Layout's spectrum_max_db). */
        int spectrum_plot_bottom;
        bool spectrum_axis_labels;
        int status_font;    /* the Freq/IF/SR/... grid on the status panel */
    } main;

    /* ---- SET page ---- */
    struct Settings {
        int margin;         /* screen edge to the cards, left and right */
        int top;            /* screen top to the first row of cards */
        int column_gap;     /* between the left and right card columns */
        int card_gap;       /* between cards stacked in a column */
        int card_pad;       /* card edge to its title, labels and buttons */
        int tall_card_h;    /* RECEIVER TUNING, DIAGNOSTICS */
        int short_card_h;   /* DISPLAY RESOLUTION, EXIT BUTTON, AUTO START */
        bool card_hints;    /* the dim one-line hints at a card's bottom */
        int label_font;
        int button_font;    /* choice buttons, SAVE & APPLY, EXIT */
        int lnb_cal_font;   /* the LNB CAL button in the card header */
        /* RECEIVER TUNING card, offsets from the card's top */
        int lo_label_y;
        int lo_box_y;
        int lo_box_h;
        int lo_value_font;  /* monospace */
        int lo_value_rise;  /* value text starts this far above the box middle */
        int voltage_label_y;
        int voltage_y;
        int voltage_w;
        int voltage_step;   /* x distance from one OFF/13V/18V button to the next */
        int voltage_h;
        /* The two-choice buttons in the short cards */
        int choice_y;
        int choice_w;
        int choice_step;
        int choice_h;
        int save_w;         /* SAVE & APPLY and EXIT */
        int save_h;
        /* DIAGNOSTICS card: text line offsets from the card's top - tuner
         * label, tuner, link label, link, VLC label, "Media > Open Network
         * Stream" (0 = not shown), VLC address. */
        int diag_font;
        int diag_y[7];
    } settings;

    /* ---- LNB calibration page ---- */
    struct LnbCal {
        int body_font;
        int emph_font;      /* the status/step line, row names */
        int mono_big_font;  /* the headline LO value */
        int value_x;        /* the corrections' values, from the left text edge */
    } lnb_cal;

    /* ---- TUNE page ---- */
    struct Tune {
        int status_row_h;
        int preset_button_h;
        int lower_cards_h;  /* the frequency / SR / RF port row */
        /* Frequency digit wheels. The seven-segment font is much wider per
         * character than its point size suggests (~0.83x), so digit_font is
         * picked to fit a glyph inside wheel_w; wheel_h is ~1.15x the font. */
        int wheel_w;
        int wheel_h;
        int digit_font;
        int dot_font;
        int freq_card_extra_w;  /* frequency card width beyond the wheels */
        int rf_card_w;
        int small_title_font;   /* "SR (kS/s) (tap)", "RF port" */
        int rf_button_font;
    } tune;
};

inline constexpr UiProfile kUiProfiles[] = {
    {
        "800x480", 800, 480,
        /* main */ {22, false, 14},
        /* settings */ {
            16, 56, 16, 12, 16, 188, 102, false,
            14, 14, 12,
            46, 70, 38, 16, 9, 122, 144, 106, 118, 38,
            40, 166, 178, 34,
            150, 38,
            14, {46, 64, 88, 106, 130, 0, 150},
        },
        /* lnb_cal */ {15, 16, 16, 236},
        /* tune */ {20, 38, 92, 46, 58, 48, 36, 8, 72, 11, 14},
    },
    {
        "1024x600", 1024, 600,
        /* main */ {36, true, 16},
        /* settings */ {
            40, 60, 24, 14, 24, 230, 124, true,
            16, 16, 14,
            50, 78, 48, 20, 11, 132, 164, 104, 116, 50,
            48, 200, 216, 40,
            200, 40,
            16, {55, 76, 109, 130, 163, 184, 201},
        },
        /* lnb_cal */ {16, 18, 20, 290},
        /* tune */ {22, 46, 128, 70, 90, 76, 56, 24, 100, 14, 20},
    },
};

inline constexpr size_t kUiProfileCount = sizeof(kUiProfiles) / sizeof(kUiProfiles[0]);

/* The row for a screen of this size: the largest one that fits it, or the
 * smallest if none does. */
inline const UiProfile & ui_profile_for(int width, int height)
{
    const UiProfile * best = &kUiProfiles[0];
    for(const UiProfile & profile : kUiProfiles) {
        if(profile.width < best->width) best = &profile;
    }
    const UiProfile * fitting = nullptr;
    for(const UiProfile & profile : kUiProfiles) {
        if(profile.width > width || profile.height > height) continue;
        if(fitting == nullptr || profile.width * profile.height > fitting->width * fitting->height)
            fitting = &profile;
    }
    return fitting != nullptr ? *fitting : *best;
}

}  // namespace qo100
