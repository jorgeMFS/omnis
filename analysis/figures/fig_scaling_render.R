#!/usr/bin/env Rscript
# Companion to analyze_omnis.py - publication-quality redraw of the
# per-instance scaling scatter (the analyze_omnis "scaling" figure),
# colored by POPULATION (S1–S4). Distinct from fig4_conservation, which
# colors the same data by solver_family.
#
# Reads analysis/discoveries.csv (the 2,383-row table the analyze script
# also uses), recomputes the pooled OLS, verifies it matches the
# manuscript, then writes to analysis/results/fig_scaling_cost_vs_length.{pdf,png}
# (overwriting the matplotlib-defaults version analyze_omnis.py emits).

suppressPackageStartupMessages({
  library(ggplot2)
})

# --- Palette (stage colours, identical to fig3 / fig5 / fig6 / fig8) ---
STAGE_COL <- c(
  S1 = "#C28166",
  S2 = "#5DA0CE",
  S3 = "#7B9358",
  S4 = "#1B3556"
)
STAGE_ALPHA <- c(S1 = 0.70, S2 = 0.55, S3 = 0.55, S4 = 0.32)
STAGE_SIZE  <- c(S1 = 0.55, S2 = 0.55, S3 = 0.55, S4 = 0.50)

PAPER     <- "#FFFFFF"
INK_AXIS  <- "#2A2A2A"
GRID_LINE <- "#EBE3D4"
SOFT_INK  <- "#5A5A5A"
OLS_LINE  <- "#2A2A2A"

# Manuscript constants (analyze_omnis.py output on the same input).
EXPECT_SLOPE <- 0.0539
EXPECT_CI_LO <- 0.0525
EXPECT_CI_HI <- 0.0554
EXPECT_R2    <- 0.6931
EXPECT_N     <- 2383L
TOL_SLOPE    <- 0.001
TOL_R2       <- 0.005

# --- Resolve paths ------------------------------------------------------
script_path <- tryCatch({
  args <- commandArgs(trailingOnly = FALSE)
  fa <- args[grep("^--file=", args)]
  if (length(fa)) normalizePath(sub("^--file=", "", fa[1])) else NA_character_
}, error = function(e) NA_character_)
here   <- if (is.na(script_path)) getwd() else dirname(script_path)
csv_in <- normalizePath(file.path(here, "..", "discoveries.csv"))
outdir <- normalizePath(file.path(here, "..", "results"))
if (!file.exists(csv_in)) stop(sprintf("missing %s", csv_in))
dir.create(outdir, showWarnings = FALSE, recursive = TRUE)

# --- Load --------------------------------------------------------------
d <- read.csv(csv_in, stringsAsFactors = FALSE)
d <- d[d$time_s > 0 & d$program_bits > 0, ]
d$log2t <- log2(d$time_s)
d$population <- factor(d$population, levels = c("S1","S2","S3","S4"))

cat(sprintf("Loaded %d discoveries from %s\n", nrow(d), basename(csv_in)))

# --- OLS + assertion gate ---------------------------------------------
fit   <- lm(log2t ~ program_bits, data = d)
ci    <- confint(fit, "program_bits", level = 0.95)
slope <- unname(coef(fit)["program_bits"])
intc  <- unname(coef(fit)["(Intercept)"])
r2    <- summary(fit)$r.squared
n     <- nrow(d)

cat(sprintf("\n  n      : %d\n", n))
cat(sprintf("  slope  : %.4f   (manuscript: %.4f)\n", slope, EXPECT_SLOPE))
cat(sprintf("  95%% CI : [%.4f, %.4f]\n", ci[1], ci[2]))
cat(sprintf("  R²     : %.4f   (manuscript: %.4f)\n", r2, EXPECT_R2))

fail <- character()
if (n != EXPECT_N)                          fail <- c(fail, sprintf("n = %d != %d", n, EXPECT_N))
if (abs(slope - EXPECT_SLOPE)  > TOL_SLOPE) fail <- c(fail, sprintf("slope %.4f != %.4f", slope, EXPECT_SLOPE))
if (abs(ci[1]  - EXPECT_CI_LO) > TOL_SLOPE) fail <- c(fail, sprintf("CI lo %.4f != %.4f", ci[1], EXPECT_CI_LO))
if (abs(ci[2]  - EXPECT_CI_HI) > TOL_SLOPE) fail <- c(fail, sprintf("CI hi %.4f != %.4f", ci[2], EXPECT_CI_HI))
if (abs(r2     - EXPECT_R2)    > TOL_R2)    fail <- c(fail, sprintf("R² %.4f != %.4f", r2, EXPECT_R2))
if (length(fail) > 0) stop(sprintf("manuscript-match gate FAILED:\n  %s",
                                   paste(fail, collapse = "\n  ")))
cat("manuscript-match gate: PASS\n")

# --- Per-population counts for legend ---------------------------------
n_by_pop <- as.integer(table(d$population)[c("S1","S2","S3","S4")])
legend_labels <- c(
  S1 = sprintf("S1  (n = %d)", n_by_pop[1]),
  S2 = sprintf("S2  (n = %d)", n_by_pop[2]),
  S3 = sprintf("S3  (n = %d)", n_by_pop[3]),
  S4 = sprintf("S4  (n = %s)", format(n_by_pop[4], big.mark = ","))
)

# --- Plot --------------------------------------------------------------
x_min <- min(d$program_bits); x_max <- max(d$program_bits)
y_min <- min(d$log2t);        y_max <- max(d$log2t)

p <- ggplot() +
  # Points in z-order S4 (densest, recedes) → S1 (sparsest, on top).
  geom_jitter(data = d[d$population == "S4", ],
              aes(x = program_bits, y = log2t, color = population),
              width = 0.4, height = 0, stroke = 0, na.rm = TRUE,
              alpha = STAGE_ALPHA[["S4"]], size = STAGE_SIZE[["S4"]]) +
  geom_jitter(data = d[d$population == "S3", ],
              aes(x = program_bits, y = log2t, color = population),
              width = 0.4, height = 0, stroke = 0, na.rm = TRUE,
              alpha = STAGE_ALPHA[["S3"]], size = STAGE_SIZE[["S3"]]) +
  geom_jitter(data = d[d$population == "S2", ],
              aes(x = program_bits, y = log2t, color = population),
              width = 0.4, height = 0, stroke = 0, na.rm = TRUE,
              alpha = STAGE_ALPHA[["S2"]], size = STAGE_SIZE[["S2"]]) +
  geom_jitter(data = d[d$population == "S1", ],
              aes(x = program_bits, y = log2t, color = population),
              width = 0.4, height = 0, stroke = 0, na.rm = TRUE,
              alpha = STAGE_ALPHA[["S1"]], size = STAGE_SIZE[["S1"]]) +

  # Pooled OLS headline line.
  geom_abline(slope = slope, intercept = intc,
              color = OLS_LINE, linewidth = 0.55) +

  # Stat block, italic soft-ink, top-left. Size 2.2 to match the inline
  # annotation scale used in fig6.
  annotate("text",
           x = x_min + 0.035 * (x_max - x_min),
           y = y_max - 0.02  * (y_max - y_min),
           label = sprintf(
"OLS  β = %.4f   95%% CI [%.4f, %.4f]
R² = %.4f      n = %s",
             slope, ci[1], ci[2], r2, format(n, big.mark = ",")),
           hjust = 0, vjust = 1,
           size = 2.2, family = "Helvetica", color = SOFT_INK,
           fontface = "italic", lineheight = 1.30) +

  scale_color_manual(values = STAGE_COL,
                     breaks = c("S1","S2","S3","S4"),
                     labels = legend_labels,
                     name = NULL,
                     guide = guide_legend(
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
    axis.title         = element_text(size = 8, color = INK_AXIS),
    axis.text          = element_text(size = 7, color = INK_AXIS),
    axis.line          = element_line(color = INK_AXIS, linewidth = 0.30),
    axis.ticks         = element_line(color = INK_AXIS, linewidth = 0.30),
    axis.ticks.length  = unit(2, "pt"),
    legend.position    = "bottom",
    legend.box         = "horizontal",
    legend.text        = element_text(size = 7, color = INK_AXIS,
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
pdf_path <- file.path(outdir, "fig_scaling_cost_vs_length.pdf")
png_path <- file.path(outdir, "fig_scaling_cost_vs_length.png")

FIG_W_MM <- 114
FIG_H_MM <- 112

ggsave(pdf_path, p, width = FIG_W_MM, height = FIG_H_MM,
       units = "mm", device = cairo_pdf)
ggsave(png_path, p, width = FIG_W_MM, height = FIG_H_MM,
       units = "mm", dpi = 300)

cat(sprintf("\nwrote %s\n      %s\n", pdf_path, png_path))
cat(sprintf("Dimensions: %d mm × %d mm\n", FIG_W_MM, FIG_H_MM))
