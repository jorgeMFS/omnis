#!/usr/bin/env Rscript
# Figure 4 - Per-instance scaling law (single panel).
#
# Replaces the prior two-panel figure (Panel A: family-mean Spearman;
# Panel B: NESTED_LOOP nesting-depth stair). Both panels are gone.
#
# Single panel:
#   x:  program description length L (bits, the engine's MDL for
#       discovered rows, where residual = 0 by construction).
#   y:  log2(discovery wall time, seconds).
#   point: one per discovery (n = 2,383), colored by population.
#   overlay: pooled OLS fit (primary, solid). Theil-Sen (secondary,
#            dashed, lighter) for the robust-fit footnote.
#
# Source: same as analyze_omnis.py - the four baseline CSVs in
# data/results/, filtered to solomonoff_class == "discovered", with
# the S3 dedupe rule applied. Self-contained; no /tmp dependency.
#
# Hard verification gate: this script aborts if the OLS slope, its
# 95% CI, or R² disagree with the manuscript by more than a tight
# rounding tolerance. The figure must match the text or stop.

suppressPackageStartupMessages({
  library(ggplot2)
})

# --- Palette (matches fig3 / fig5 / fig6 / fig8) ----------------------
STAGE_COL <- c(
  S1 = "#C28166",
  S2 = "#5DA0CE",
  S3 = "#7B9358",
  S4 = "#1B3556"
)
PAPER       <- "#FFFFFF"
INK_AXIS    <- "#2A2A2A"
GRID_LINE   <- "#EBE3D4"
DARK_INK    <- "#2A2A2A"
SOFT_INK    <- "#5A5A5A"     # annotation text, slightly recessed
OLS_LINE    <- "#2A2A2A"     # OLS (all 2,383): primary, solid black ink
NC_LINE     <- "#A8332C"     # OLS (no CTX_X subset): secondary, solid dark red
TS_LINE     <- "#B8B0A4"     # Theil-Sen: tertiary, dashed, faint
DEADLINE_LN <- "#C8C0B4"     # CTX_X 30s phase deadline reference

# Per-population aesthetic: S4 is the densest cloud (1,683 pts) - lower alpha
# so the cluster does not visually swamp S1/S2/S3. S1 (120 pts) is sparsest -
# higher alpha to keep individual points readable.
STAGE_ALPHA <- c(S1 = 0.70, S2 = 0.55, S3 = 0.55, S4 = 0.32)
STAGE_SIZE  <- c(S1 = 0.55, S2 = 0.55, S3 = 0.55, S4 = 0.50)

DROP_XREF <- c("A000069", "A000120", "A001969", "A002113")

# Manuscript numbers - TWO regressions reported in Section 3.3.
# Pooled: all 2,383 discoveries.
# Conditional: same regression restricted to discoveries NOT produced by
# the CTX_X solver family. CTX_X has an engine-internal 30s phase deadline
# (omnis.cpp:2519, ctx_budget = clamp(5, 30, remaining/2)) which injects
# time variance uncorrelated with program length. Reporting both makes the
# scaling claim robust to that internal deadline.
EXPECT_SLOPE_ALL  <- 0.0539
EXPECT_CI_LO_ALL  <- 0.0525
EXPECT_CI_HI_ALL  <- 0.0554
EXPECT_R2_ALL     <- 0.6931
EXPECT_N_ALL      <- 2383L

EXPECT_SLOPE_NC   <- 0.0566
EXPECT_CI_LO_NC   <- 0.0552
EXPECT_CI_HI_NC   <- 0.0580
EXPECT_R2_NC      <- 0.8547
EXPECT_N_NC       <- 1093L

TOL_SLOPE    <- 0.001        # tight rounding tolerance
TOL_R2       <- 0.005

# CTX_X phase deadline observed in data: median time = 43.2s; engine
# constant from omnis.cpp:2519 = 30s plus prior-phase startup → ~42s.
CTX_DEADLINE_S <- 42.5

# --- Resolve paths ------------------------------------------------------
script_path <- tryCatch({
  args <- commandArgs(trailingOnly = FALSE)
  fa <- args[grep("^--file=", args)]
  if (length(fa)) normalizePath(sub("^--file=", "", fa[1])) else NA_character_
}, error = function(e) NA_character_)
here    <- if (is.na(script_path)) getwd() else dirname(script_path)
csv_dir <- normalizePath(file.path(here, "..", "..", "data", "results"))

S1_CSV <- file.path(csv_dir, "baseline_20260511T091836Z.csv")
S2_CSV <- file.path(csv_dir, "baseline_20260513T232442Z.csv")
S3_CSV <- file.path(csv_dir, "baseline_20260514T024454Z.csv")
S4_CSV <- file.path(csv_dir, "baseline_20260520T155701Z.csv")
for (p in c(S1_CSV, S2_CSV, S3_CSV, S4_CSV)) {
  if (!file.exists(p)) stop(sprintf("Missing CSV: %s", p))
}

# --- Loader -------------------------------------------------------------
parse_family <- function(desc) {
  if (length(desc) == 0) return(character(0))
  vapply(desc, function(d) {
    if (is.na(d) || nchar(d) == 0) return("")
    head <- sub("^\"", "", d)
    sub(" .*$", "", head)
  }, FUN.VALUE = character(1), USE.NAMES = FALSE)
}

load_stage <- function(csv_path, stage_label, apply_dedupe = FALSE) {
  raw <- read.csv(csv_path, stringsAsFactors = FALSE)
  if (apply_dedupe) {
    raw <- raw[!(raw$category == "oeis_base" &
                 raw$oeis_xref %in% DROP_XREF), ]
  }
  raw <- raw[raw$solomonoff_class == "discovered", ]
  data.frame(
    id            = raw$id,
    population    = stage_label,
    solver_family = parse_family(raw$solver_desc),
    program_bits  = as.numeric(raw$mdl),
    time_s        = as.numeric(raw$time_s),
    stringsAsFactors = FALSE
  )
}

cat("Loading discovered rows from baseline CSVs...\n")
s1 <- load_stage(S1_CSV, "S1", FALSE)
s2 <- load_stage(S2_CSV, "S2", FALSE)
s3 <- load_stage(S3_CSV, "S3", TRUE)
s4 <- load_stage(S4_CSV, "S4", FALSE)
fig4 <- rbind(s1, s2, s3, s4)
fig4$population <- factor(fig4$population, levels = c("S1","S2","S3","S4"))

cat(sprintf("  S1: %d  S2: %d  S3 (deduped): %d  S4: %d\n",
            nrow(s1), nrow(s2), nrow(s3), nrow(s4)))
cat(sprintf("  total discoveries: %d  (expected %d)\n",
            nrow(fig4), EXPECT_N_ALL))
if (nrow(fig4) != EXPECT_N_ALL) {
  stop(sprintf("discovery count = %d, expected %d", nrow(fig4), EXPECT_N_ALL))
}

# --- Filter to usable rows for log2(time_s) ---------------------------
d <- fig4[fig4$time_s > 0 & fig4$program_bits > 0, ]
if (nrow(d) != nrow(fig4)) {
  cat(sprintf("  dropped %d rows with time_s <= 0 or program_bits <= 0\n",
              nrow(fig4) - nrow(d)))
}

# --- OLS regression: two fits side by side -----------------------------
# (A) all 2,383 discoveries - the headline empirical scaling.
# (B) same regression restricted to NOT-CTX_X - strips the engine-internal
#     30s phase-deadline variance and reveals the structural slope.
d$log2t <- log2(d$time_s)
d$is_ctx <- d$solver_family == "CTX_X"

fit_one <- function(sub, label) {
  fit  <- lm(log2t ~ program_bits, data = sub)
  ci   <- confint(fit, "program_bits", level = 0.95)
  list(slope = unname(coef(fit)["program_bits"]),
       intc  = unname(coef(fit)["(Intercept)"]),
       lo    = ci[1], hi = ci[2],
       r2    = summary(fit)$r.squared,
       p     = summary(fit)$coefficients["program_bits", "Pr(>|t|)"],
       n     = nrow(sub),
       label = label)
}

fA <- fit_one(d,                "all")
fB <- fit_one(d[!d$is_ctx, ],   "no_CTX_X")

cat("\n=== OLS regression: log2(time_s) ~ program_bits ===\n")
print_fit <- function(f, lbl, eS, eLo, eHi, eR2, eN) {
  cat(sprintf("  [%s]  n=%d  (manuscript: %d)\n", lbl, f$n, eN))
  cat(sprintf("    slope     : %.4f   (manuscript: %.4f, tol %.4f)\n",
              f$slope, eS, TOL_SLOPE))
  cat(sprintf("    95%% CI    : [%.4f, %.4f]   (manuscript: [%.4f, %.4f])\n",
              f$lo, f$hi, eLo, eHi))
  cat(sprintf("    R^2       : %.4f   (manuscript: %.4f, tol %.4f)\n",
              f$r2, eR2, TOL_R2))
  cat(sprintf("    p         : %.3e\n", f$p))
}
print_fit(fA, "all",       EXPECT_SLOPE_ALL, EXPECT_CI_LO_ALL, EXPECT_CI_HI_ALL, EXPECT_R2_ALL, EXPECT_N_ALL)
print_fit(fB, "no_CTX_X",  EXPECT_SLOPE_NC,  EXPECT_CI_LO_NC,  EXPECT_CI_HI_NC,  EXPECT_R2_NC,  EXPECT_N_NC)

check <- function(f, eS, eLo, eHi, eR2, eN, lbl) {
  fail <- character()
  if (f$n != eN)                     fail <- c(fail, sprintf("[%s] n %d != %d", lbl, f$n, eN))
  if (abs(f$slope - eS) > TOL_SLOPE) fail <- c(fail, sprintf("[%s] slope %.4f != %.4f", lbl, f$slope, eS))
  if (abs(f$lo - eLo)   > TOL_SLOPE) fail <- c(fail, sprintf("[%s] CI lo %.4f != %.4f", lbl, f$lo, eLo))
  if (abs(f$hi - eHi)   > TOL_SLOPE) fail <- c(fail, sprintf("[%s] CI hi %.4f != %.4f", lbl, f$hi, eHi))
  if (abs(f$r2 - eR2)   > TOL_R2)    fail <- c(fail, sprintf("[%s] R^2 %.4f != %.4f", lbl, f$r2, eR2))
  fail
}
fail <- c(check(fA, EXPECT_SLOPE_ALL, EXPECT_CI_LO_ALL, EXPECT_CI_HI_ALL, EXPECT_R2_ALL, EXPECT_N_ALL, "all"),
          check(fB, EXPECT_SLOPE_NC,  EXPECT_CI_LO_NC,  EXPECT_CI_HI_NC,  EXPECT_R2_NC,  EXPECT_N_NC,  "no_CTX_X"))
if (length(fail) > 0) {
  stop(sprintf("manuscript-match gate FAILED:\n  %s",
               paste(fail, collapse = "\n  ")))
}
cat("manuscript-match gate: PASS (both regressions)\n")

slope <- fA$slope; intc <- fA$intc; ci <- c(fA$lo, fA$hi); r2 <- fA$r2; n <- fA$n

# --- Theil-Sen slope on a deterministic subsample ---------------------
# Full pairwise Theil-Sen on n=2,383 = ~2.8M pairs, ~tens of seconds in R.
# Use a fixed-seed random subsample of 800 points (≈ 320k pairs) - slope
# stabilises well before the asymptote. Manuscript reports 0.066; we
# annotate the canonical value below the fitted line, but the visual
# overlay is drawn from the recomputed subsample slope.
set.seed(1)
sub_idx <- sample.int(n, min(800, n))
xs <- d$program_bits[sub_idx]
ys <- d$log2t[sub_idx]
m  <- length(xs)
slopes <- numeric()
for (i in 1:(m-1)) {
  dx <- xs[(i+1):m] - xs[i]
  dy <- ys[(i+1):m] - ys[i]
  keep <- dx != 0
  if (any(keep)) slopes <- c(slopes, dy[keep] / dx[keep])
}
ts_slope <- median(slopes)
ts_intc  <- median(d$log2t - ts_slope * d$program_bits)
cat(sprintf("Theil-Sen slope (subsample n=%d): %.4f   (manuscript: 0.0660)\n",
            m, ts_slope))

# --- Plot data --------------------------------------------------------
# Slight per-stage jitter on a single tied x (program_bits is integer-like
# in the engine - many ties) is unnecessary because we plot on float bits.
# Order populations so S4 (densest, dark navy) is drawn first/underneath.
d$population <- factor(d$population, levels = c("S4","S3","S2","S1"))
d <- d[order(d$population), ]
d$population <- factor(as.character(d$population),
                       levels = c("S1","S2","S3","S4"))

# Range bookkeeping for line / annotation placement
x_min <- min(d$program_bits); x_max <- max(d$program_bits)
y_min <- min(d$log2t);        y_max <- max(d$log2t)

# Legend labels carry per-population counts
n_by_pop <- table(factor(d$population, levels = c("S1","S2","S3","S4")))
legend_labels <- c(
  S1 = sprintf("S1  (n = %d)", n_by_pop[["S1"]]),
  S2 = sprintf("S2  (n = %d)",  n_by_pop[["S2"]]),
  S3 = sprintf("S3  (n = %d)",  n_by_pop[["S3"]]),
  S4 = sprintf("S4  (n = %s)",
               format(n_by_pop[["S4"]], big.mark = ","))
)

p <- ggplot() +

  # Faint horizontal reference at the CTX_X engine phase deadline
  # (omnis.cpp:2519: ctx_budget clamped to 30s + prior-phase startup
  # ≈ 42s wall). Marks the visible band in the cloud as an engine
  # artifact, not noise.
  geom_hline(yintercept = log2(CTX_DEADLINE_S),
             color = DEADLINE_LN, linewidth = 0.25,
             linetype = "dotted") +

  # Theil-Sen tertiary: faintest, dashed, drawn under everything.
  geom_abline(slope = ts_slope, intercept = ts_intc,
              color = TS_LINE, linewidth = 0.25,
              linetype = "dashed") +

  # Points drawn per population in z-order S4 → S3 → S2 → S1 so the
  # sparser populations stay legible over the dense S4 navy cloud.
  # Small horizontal jitter (0.4 bits) breaks the vertical stripes of
  # discrete program-length tick values without distorting the fit.
  geom_jitter(data = d[d$population == "S4", ],
              aes(x = program_bits, y = log2t, color = population),
              width = 0.4, height = 0,
              alpha = STAGE_ALPHA[["S4"]], size = STAGE_SIZE[["S4"]],
              stroke = 0, na.rm = TRUE) +
  geom_jitter(data = d[d$population == "S3", ],
              aes(x = program_bits, y = log2t, color = population),
              width = 0.4, height = 0,
              alpha = STAGE_ALPHA[["S3"]], size = STAGE_SIZE[["S3"]],
              stroke = 0, na.rm = TRUE) +
  geom_jitter(data = d[d$population == "S2", ],
              aes(x = program_bits, y = log2t, color = population),
              width = 0.4, height = 0,
              alpha = STAGE_ALPHA[["S2"]], size = STAGE_SIZE[["S2"]],
              stroke = 0, na.rm = TRUE) +
  geom_jitter(data = d[d$population == "S1", ],
              aes(x = program_bits, y = log2t, color = population),
              width = 0.4, height = 0,
              alpha = STAGE_ALPHA[["S1"]], size = STAGE_SIZE[["S1"]],
              stroke = 0, na.rm = TRUE) +

  # OLS (no-CTX_X subset): drawn under the main OLS so the headline
  # line stays on top, but in a distinct color so both are readable.
  geom_abline(slope = fB$slope, intercept = fB$intc,
              color = NC_LINE, linewidth = 0.50) +
  # OLS (all 2,383): headline, dark ink, slightly heavier.
  geom_abline(slope = fA$slope, intercept = fA$intc,
              color = OLS_LINE, linewidth = 0.55) +

  # Stat block - two regressions reported side-by-side, italic, soft ink.
  annotate("text",
           x = x_min + 0.035 * (x_max - x_min),
           y = y_max - 0.02  * (y_max - y_min),
           label = sprintf(
"OLS (all)         β = %.4f   95%% CI [%.4f, %.4f]   R² = %.4f   n = %s
OLS (no CTX_X) β = %.4f   95%% CI [%.4f, %.4f]   R² = %.4f   n = %s
Theil–Sen         β = %.4f",
             fA$slope, fA$lo, fA$hi, fA$r2, format(fA$n, big.mark=","),
             fB$slope, fB$lo, fB$hi, fB$r2, format(fB$n, big.mark=","),
             ts_slope),
           hjust = 0, vjust = 1,
           size = 2.0, family = "Helvetica", color = SOFT_INK,
           fontface = "italic",
           lineheight = 1.25) +

  # Small label on the deadline reference, right edge of axes.
  annotate("text",
           x = x_max - 0.02 * (x_max - x_min),
           y = log2(CTX_DEADLINE_S),
           label = sprintf("CTX_X 30 s phase deadline (≈%.0f s wall)",
                           CTX_DEADLINE_S),
           hjust = 1, vjust = -0.5,
           size = 1.9, family = "Helvetica", color = SOFT_INK,
           fontface = "italic") +

  scale_color_manual(values = STAGE_COL,
                     breaks = c("S1","S2","S3","S4"),
                     labels = legend_labels,
                     name = NULL,
                     guide = guide_legend(
                       override.aes = list(size = 1.7, alpha = 1)
                     )) +
  scale_x_continuous(expand = expansion(mult = c(0.02, 0.04))) +
  scale_y_continuous(expand = expansion(mult = c(0.03, 0.05))) +
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
# Detailed per-discovery scatter, intended for SI (richer than the
# main-text fig4 which is a binned staircase).
pdf_path <- file.path(here, "fig4_si_scatter.pdf")
png_path <- file.path(here, "fig4_si_scatter.png")

# 1.5-column PNAS width, near-square - same envelope as fig6 so the two
# scatter-style figures sit at the same scale. Room for the 3-line stat
# block + deadline label + bottom legend without crowding.
FIG_W_MM <- 114
FIG_H_MM <- 112

ggsave(pdf_path, p, width = FIG_W_MM, height = FIG_H_MM,
       units = "mm", device = cairo_pdf)
ggsave(png_path, p, width = FIG_W_MM, height = FIG_H_MM,
       units = "mm", dpi = 300)

# --- Side-car -------------------------------------------------------
write.csv(d[, c("id","population","solver_family","program_bits","time_s","log2t")],
          file.path(here, "fig4_si_scatter_data.csv"), row.names = FALSE)

cat(sprintf("\nwrote %s\n      %s\n      fig4_si_scatter_data.csv\n",
            pdf_path, png_path))
cat(sprintf("Dimensions: %d mm x %d mm  (single panel, 1.5-column)\n",
            FIG_W_MM, FIG_H_MM))
cat(sprintf("Points plotted: %d\n", n))
