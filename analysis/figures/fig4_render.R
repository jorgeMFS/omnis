#!/usr/bin/env Rscript
# Figure 4 — Per-instance scaling, coloured by engine phase.
#
# Replot of analysis/figures/fig4_data.csv (2,383 discoveries). Does not
# re-run the engine, does not read baseline CSVs, does not touch the null
# sweep. Reads the CSV, recomputes the two OLS regressions, asserts they
# match the manuscript, and writes the PDF + PNG.
#
# Visual hypothesis: the horizontal bands in the cloud are phase
# deadlines. Colouring by solver_family makes that architecture explicit
# — CTX_X sits at the 30 s phase deadline, NESTED_LOOP at the Phase 2H
# budget around 200 s. What looked like noise on a slope is the engine's
# scheduler.

suppressPackageStartupMessages({
  library(ggplot2)
})

# --- Palette ------------------------------------------------------------
# Solver-family colours. Distinct from the S1–S4 population palette so the
# two encodings cannot be confused; restrained saturation so the dense
# cloud reads as architecture, not as a riot.
FAM_COL <- c(
  CTX_X       = "#3A8081",   # teal — the dominant phase (54% of discoveries)
  DARY        = "#C19850",   # gold — the deductive cohort
  NESTED_LOOP = "#C76746",   # warm terracotta — the Phase 2H cluster
  FUNC_L      = "#7062A5",   # soft violet
  FUNC        = "#9E6378",   # dusty plum
  Other       = "#ADA39C"    # warm gray — the small-family bucket
)
# Z-order: dense families drawn first (recede), sparse families on top.
FAM_ORDER  <- c("CTX_X", "DARY", "NESTED_LOOP", "FUNC", "FUNC_L", "Other")
FAM_ALPHA  <- c(CTX_X = 0.32, DARY = 0.34, NESTED_LOOP = 0.55,
                FUNC = 0.65, FUNC_L = 0.65, Other = 0.70)
FAM_SIZE   <- c(CTX_X = 0.50, DARY = 0.50, NESTED_LOOP = 0.60,
                FUNC = 0.60, FUNC_L = 0.60, Other = 0.60)

PAPER       <- "#FFFFFF"
INK_AXIS    <- "#2A2A2A"
GRID_LINE   <- "#EBE3D4"
SOFT_INK    <- "#5A5A5A"
DARK_INK    <- "#2A2A2A"
OLS_LINE    <- "#2A2A2A"
DEADLINE_LN <- "#C8C0B4"

# --- Manuscript constants (re-checked against the CSV below) ----------
EXPECT_SLOPE_ALL  <- 0.0539
EXPECT_CI_LO_ALL  <- 0.0525
EXPECT_CI_HI_ALL  <- 0.0554
EXPECT_R2_ALL     <- 0.6931
EXPECT_N_ALL      <- 2383L

EXPECT_SLOPE_NC   <- 0.0566
EXPECT_R2_NC      <- 0.8547
EXPECT_N_NC       <- 1093L

TOL_SLOPE         <- 0.001
TOL_R2            <- 0.005

# Phase-deadline reference (s) — only the CTX_X line is derivable from
# the engine source (omnis.cpp:2519, ctx_budget clamped to 30s plus
# prior-phase startup ≈ 42s wall). The ~200s NESTED_LOOP cluster is left
# unlabelled: it is the empirical median wall of Phase-2F WSBP enumeration
# reaching L=5..8 NESTED_LOOP bodies (omnis.cpp:3838), not a budget cap,
# and so it would be misleading to draw it as a "phase deadline" line.
# The cluster is visible by the terracotta NESTED_LOOP points themselves.
CTX_DEADLINE_S    <- 42

# --- Resolve paths ------------------------------------------------------
script_path <- tryCatch({
  args <- commandArgs(trailingOnly = FALSE)
  fa <- args[grep("^--file=", args)]
  if (length(fa)) normalizePath(sub("^--file=", "", fa[1])) else NA_character_
}, error = function(e) NA_character_)
here <- if (is.na(script_path)) getwd() else dirname(script_path)

csv_in <- file.path(here, "fig4_data.csv")
if (!file.exists(csv_in)) stop(sprintf("missing %s", csv_in))

# --- Load --------------------------------------------------------------
d <- read.csv(csv_in, stringsAsFactors = FALSE)
needed <- c("solver_family", "program_bits", "time_s")
miss <- setdiff(needed, names(d))
if (length(miss)) stop(sprintf("fig4_data.csv missing columns: %s",
                               paste(miss, collapse = ", ")))

d <- d[d$time_s > 0 & d$program_bits > 0, ]
d$log2t <- log2(d$time_s)

cat(sprintf("Loaded %d discoveries from %s\n", nrow(d), basename(csv_in)))

# --- Assertion gate: recompute both regressions -----------------------
fit_one <- function(sub) {
  fit  <- lm(log2t ~ program_bits, data = sub)
  ci   <- confint(fit, "program_bits", level = 0.95)
  list(slope = unname(coef(fit)["program_bits"]),
       intc  = unname(coef(fit)["(Intercept)"]),
       lo    = ci[1], hi = ci[2],
       r2    = summary(fit)$r.squared,
       n     = nrow(sub))
}
fA <- fit_one(d)
fB <- fit_one(d[d$solver_family != "CTX_X", ])

cat("\n=== OLS verification against manuscript ===\n")
cat(sprintf("  [all]       n=%d  β=%.4f  CI=[%.4f,%.4f]  R²=%.4f\n",
            fA$n, fA$slope, fA$lo, fA$hi, fA$r2))
cat(sprintf("              manuscript: n=%d  β=%.4f  CI=[%.4f,%.4f]  R²=%.4f\n",
            EXPECT_N_ALL, EXPECT_SLOPE_ALL, EXPECT_CI_LO_ALL,
            EXPECT_CI_HI_ALL, EXPECT_R2_ALL))
cat(sprintf("  [no CTX_X]  n=%d  β=%.4f  R²=%.4f\n",
            fB$n, fB$slope, fB$r2))
cat(sprintf("              manuscript: n=%d  β=%.4f  R²=%.4f\n",
            EXPECT_N_NC, EXPECT_SLOPE_NC, EXPECT_R2_NC))

fail <- character()
if (fA$n != EXPECT_N_ALL)
  fail <- c(fail, sprintf("[all] n=%d != %d",       fA$n, EXPECT_N_ALL))
if (abs(fA$slope - EXPECT_SLOPE_ALL) > TOL_SLOPE)
  fail <- c(fail, sprintf("[all] slope %.4f != %.4f",  fA$slope, EXPECT_SLOPE_ALL))
if (abs(fA$lo - EXPECT_CI_LO_ALL) > TOL_SLOPE)
  fail <- c(fail, sprintf("[all] CI lo %.4f != %.4f",  fA$lo, EXPECT_CI_LO_ALL))
if (abs(fA$hi - EXPECT_CI_HI_ALL) > TOL_SLOPE)
  fail <- c(fail, sprintf("[all] CI hi %.4f != %.4f",  fA$hi, EXPECT_CI_HI_ALL))
if (abs(fA$r2 - EXPECT_R2_ALL) > TOL_R2)
  fail <- c(fail, sprintf("[all] R² %.4f != %.4f",     fA$r2, EXPECT_R2_ALL))
if (fB$n != EXPECT_N_NC)
  fail <- c(fail, sprintf("[no CTX_X] n=%d != %d",  fB$n, EXPECT_N_NC))
if (abs(fB$slope - EXPECT_SLOPE_NC) > TOL_SLOPE)
  fail <- c(fail, sprintf("[no CTX_X] slope %.4f != %.4f", fB$slope, EXPECT_SLOPE_NC))
if (abs(fB$r2 - EXPECT_R2_NC) > TOL_R2)
  fail <- c(fail, sprintf("[no CTX_X] R² %.4f != %.4f",    fB$r2, EXPECT_R2_NC))

if (length(fail) > 0) {
  stop(sprintf("manuscript-match gate FAILED — figure not written:\n  %s",
               paste(fail, collapse = "\n  ")))
}
cat("manuscript-match gate: PASS\n")

# --- Family bucketing -------------------------------------------------
big <- c("CTX_X","DARY","NESTED_LOOP","FUNC","FUNC_L")
d$family <- ifelse(d$solver_family %in% big, d$solver_family, "Other")
d$family <- factor(d$family, levels = FAM_ORDER)

# Counts for the legend
fam_counts <- as.integer(table(d$family)[FAM_ORDER])
legend_labels <- vapply(seq_along(FAM_ORDER), function(i) {
  n <- fam_counts[i]
  if (FAM_ORDER[i] %in% c("CTX_X","NESTED_LOOP")) {
    # the two named phase bands — slightly emphasised label
    sprintf("%s  (n = %s)", FAM_ORDER[i], format(n, big.mark = ","))
  } else {
    sprintf("%s  (n = %s)", FAM_ORDER[i], format(n, big.mark = ","))
  }
}, character(1))
names(legend_labels) <- FAM_ORDER

# --- Plot --------------------------------------------------------------
x_min <- min(d$program_bits); x_max <- max(d$program_bits)
y_min <- min(d$log2t);        y_max <- max(d$log2t)

p <- ggplot() +

  # Phase-deadline reference — only CTX_X, the one constant derivable from
  # the engine source. Faint dotted line + short italic label.
  geom_hline(yintercept = log2(CTX_DEADLINE_S),
             color = DEADLINE_LN, linewidth = 0.22, linetype = "dotted") +

  # Points — drawn in family order with stage-tuned alpha/size, plus a
  # small horizontal jitter to break the vertical stripes of discrete
  # program-length values.
  geom_jitter(data = d[d$family == "CTX_X", ],
              aes(x = program_bits, y = log2t, color = family),
              width = 0.4, height = 0, stroke = 0, na.rm = TRUE,
              alpha = FAM_ALPHA[["CTX_X"]], size = FAM_SIZE[["CTX_X"]]) +
  geom_jitter(data = d[d$family == "DARY", ],
              aes(x = program_bits, y = log2t, color = family),
              width = 0.4, height = 0, stroke = 0, na.rm = TRUE,
              alpha = FAM_ALPHA[["DARY"]], size = FAM_SIZE[["DARY"]]) +
  geom_jitter(data = d[d$family == "NESTED_LOOP", ],
              aes(x = program_bits, y = log2t, color = family),
              width = 0.4, height = 0, stroke = 0, na.rm = TRUE,
              alpha = FAM_ALPHA[["NESTED_LOOP"]], size = FAM_SIZE[["NESTED_LOOP"]]) +
  geom_jitter(data = d[d$family == "FUNC", ],
              aes(x = program_bits, y = log2t, color = family),
              width = 0.4, height = 0, stroke = 0, na.rm = TRUE,
              alpha = FAM_ALPHA[["FUNC"]], size = FAM_SIZE[["FUNC"]]) +
  geom_jitter(data = d[d$family == "FUNC_L", ],
              aes(x = program_bits, y = log2t, color = family),
              width = 0.4, height = 0, stroke = 0, na.rm = TRUE,
              alpha = FAM_ALPHA[["FUNC_L"]], size = FAM_SIZE[["FUNC_L"]]) +
  geom_jitter(data = d[d$family == "Other", ],
              aes(x = program_bits, y = log2t, color = family),
              width = 0.4, height = 0, stroke = 0, na.rm = TRUE,
              alpha = FAM_ALPHA[["Other"]], size = FAM_SIZE[["Other"]]) +

  # OLS headline line.
  geom_abline(slope = fA$slope, intercept = fA$intc,
              color = OLS_LINE, linewidth = 0.55) +

  # Stat block: two raw OLS measurements, italic soft ink, tight leading.
  # The interpretive "cost rises ≈ X× per bit" line was removed because
  # the manuscript body no longer states a precise per-bit multiplier
  # (deliberate, after the time-axis censoring discussion). The two
  # regressions stand on their own as raw measurements.
  annotate("text",
           x = x_min + 0.035 * (x_max - x_min),
           y = y_max - 0.02  * (y_max - y_min),
           label = sprintf(
"OLS (all)         β = %.4f   95%% CI [%.4f, %.4f]   R² = %.2f   n = %s
OLS (no CTX_X) β = %.4f                              R² = %.2f   n = %s",
             fA$slope, fA$lo, fA$hi, fA$r2, format(fA$n, big.mark = ","),
             fB$slope, fB$r2, format(fB$n, big.mark = ",")),
           hjust = 0, vjust = 1,
           size = 2.05, family = "Helvetica", color = SOFT_INK,
           fontface = "italic", lineheight = 1.30) +

  # Single phase-deadline label on the right edge of the reference line.
  annotate("text",
           x = x_max - 0.01 * (x_max - x_min),
           y = log2(CTX_DEADLINE_S),
           label = sprintf("CTX_X 30 s phase deadline  (≈%d s)", CTX_DEADLINE_S),
           hjust = 1, vjust = -0.50,
           size = 1.95, family = "Helvetica", color = SOFT_INK,
           fontface = "italic") +

  scale_color_manual(values = FAM_COL,
                     breaks = FAM_ORDER,
                     labels = legend_labels,
                     name = NULL,
                     guide = guide_legend(
                       nrow = 2, byrow = TRUE,
                       override.aes = list(size = 1.7, alpha = 1, stroke = 0)
                     )) +
  scale_x_continuous(expand = expansion(mult = c(0.02, 0.03))) +
  scale_y_continuous(expand = expansion(mult = c(0.02, 0.05))) +
  labs(x = "Program description length (bits)",
       y = expression(log[2]~"discovery time (s)")) +
  theme_minimal(base_family = "Helvetica") +
  theme(
    panel.grid.minor   = element_blank(),
    panel.grid.major   = element_line(color = GRID_LINE, linewidth = 0.18),
    axis.title         = element_text(size = 7.5, color = INK_AXIS),
    axis.text          = element_text(size = 6.5, color = INK_AXIS),
    axis.line          = element_line(color = INK_AXIS, linewidth = 0.30),
    axis.ticks         = element_line(color = INK_AXIS, linewidth = 0.30),
    axis.ticks.length  = unit(2, "pt"),
    legend.position    = "bottom",
    legend.box         = "horizontal",
    legend.text        = element_text(size = 6.5, color = INK_AXIS,
                                      family = "Helvetica"),
    legend.key         = element_rect(fill = NA, color = NA),
    legend.key.size    = unit(3.0, "mm"),
    legend.spacing.x   = unit(4, "pt"),
    legend.margin      = margin(2, 0, 0, 0),
    plot.background    = element_rect(fill = PAPER, color = NA),
    panel.background   = element_rect(fill = PAPER, color = NA),
    plot.margin        = margin(8, 10, 4, 8)
  )

# --- Save -----------------------------------------------------------
pdf_path <- file.path(here, "fig4_conservation.pdf")
png_path <- file.path(here, "fig4_conservation.png")

FIG_W_MM <- 114
FIG_H_MM <- 112

ggsave(pdf_path, p, width = FIG_W_MM, height = FIG_H_MM,
       units = "mm", device = cairo_pdf)
ggsave(png_path, p, width = FIG_W_MM, height = FIG_H_MM,
       units = "mm", dpi = 300)

cat(sprintf("\nwrote %s\n      %s\n", pdf_path, png_path))
cat(sprintf("Dimensions: %d mm × %d mm  (single panel, 1.5-column)\n",
            FIG_W_MM, FIG_H_MM))
cat(sprintf("Points plotted: %d\n", nrow(d)))
cat("Family counts:\n")
for (f in FAM_ORDER) {
  cat(sprintf("  %s: %d\n", f, sum(d$family == f)))
}
