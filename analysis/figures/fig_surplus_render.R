#!/usr/bin/env Rscript
# Companion to analyze_omnis.py - publication-quality redraw of the
# gate false-acceptance surplus histogram.
#
# Per-discovery surplus  Δ = m · log₂|Σ| − L   (in bits)
# where m = holdout_len, |Σ| = alphabet_size, L = program_bits.
# The per-instance analytical FAP bound is 2^−(Δ−1); positive surplus
# means the bound is informative (< 1/2), negative means it is vacuous.
#
# Reads analysis/discoveries.csv, computes Δ per row, asserts the summary
# stats match the manuscript, writes to analysis/results/fig_gate_surplus.{pdf,png}.

suppressPackageStartupMessages({
  library(ggplot2)
})

# --- Palette (matches fig4 / fig6) -------------------------------------
PAPER     <- "#FFFFFF"
INK_AXIS  <- "#2A2A2A"
GRID_LINE <- "#EBE3D4"
SOFT_INK  <- "#5A5A5A"
DARK_INK  <- "#2A2A2A"

BAR_FILL  <- "#7E8FA8"   # quiet slate — single neutral fill, no paper
                         # palette conflict.
ZERO_LINE <- "#A8332C"   # dark red, dashed — Δ = 0 boundary

# Manuscript stats (from analyze_omnis.py over the same input).
EXPECT_MIN    <- -75.6
EXPECT_MEDIAN <- 173.4
EXPECT_MAX    <- 184.7
EXPECT_FPOS   <- 0.9660
EXPECT_N      <- 2383L
TOL_BITS      <- 0.5
TOL_FRAC      <- 0.005

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

# --- Load + compute surplus -------------------------------------------
d <- read.csv(csv_in, stringsAsFactors = FALSE)
d <- d[d$holdout_len > 0 & d$alphabet_size > 1 & d$program_bits > 0, ]
d$surplus <- d$holdout_len * log2(d$alphabet_size) - d$program_bits

n      <- nrow(d)
s_min  <- min(d$surplus)
s_med  <- median(d$surplus)
s_max  <- max(d$surplus)
f_pos  <- mean(d$surplus > 0)

cat(sprintf("Loaded %d discoveries from %s\n", n, basename(csv_in)))
cat(sprintf("\n  surplus min    : %.1f bits   (manuscript: %.1f)\n",
            s_min, EXPECT_MIN))
cat(sprintf("  surplus median : %.1f bits   (manuscript: %.1f)\n",
            s_med, EXPECT_MEDIAN))
cat(sprintf("  surplus max    : %.1f bits   (manuscript: %.1f)\n",
            s_max, EXPECT_MAX))
cat(sprintf("  fraction Δ>0   : %.4f       (manuscript: %.4f)\n",
            f_pos, EXPECT_FPOS))

fail <- character()
if (n != EXPECT_N)                          fail <- c(fail, sprintf("n = %d != %d", n, EXPECT_N))
if (abs(s_min - EXPECT_MIN)    > TOL_BITS)  fail <- c(fail, sprintf("min %.1f != %.1f", s_min, EXPECT_MIN))
if (abs(s_med - EXPECT_MEDIAN) > TOL_BITS)  fail <- c(fail, sprintf("median %.1f != %.1f", s_med, EXPECT_MEDIAN))
if (abs(s_max - EXPECT_MAX)    > TOL_BITS)  fail <- c(fail, sprintf("max %.1f != %.1f", s_max, EXPECT_MAX))
if (abs(f_pos - EXPECT_FPOS)   > TOL_FRAC)  fail <- c(fail, sprintf("frac>0 %.4f != %.4f", f_pos, EXPECT_FPOS))
if (length(fail) > 0) stop(sprintf("manuscript-match gate FAILED:\n  %s",
                                   paste(fail, collapse = "\n  ")))
cat("manuscript-match gate: PASS\n")

# --- Plot --------------------------------------------------------------
# 5-bit bins ⇒ ~53 bins across [-76, +185]: enough resolution to see the
# 3.4% negative-surplus tail without fragmenting the positive mode.
bin_w <- 5
brk   <- seq(floor(s_min / bin_w) * bin_w,
             ceiling(s_max / bin_w) * bin_w + bin_w,
             by = bin_w)

h <- hist(d$surplus, breaks = brk, plot = FALSE)
y_max <- max(h$counts)

p <- ggplot(d, aes(x = surplus)) +

  # Histogram - single neutral fill, no per-bar outlines, breathes against grid.
  geom_histogram(breaks = brk,
                 fill = BAR_FILL, color = NA, alpha = 0.92) +

  # Δ = 0 reference: the sound/vacuous boundary.
  geom_vline(xintercept = 0, color = ZERO_LINE,
             linewidth = 0.40, linetype = "dashed") +

  # Single stat block, top-right, italic soft-ink. Tells the whole story:
  # range, fraction sound, n. No competing inline labels.
  annotate("text",
           x = s_max - 0.02 * (s_max - s_min),
           y = y_max * 1.02,
           label = sprintf(
"Δ_min = %.1f       Δ_median = %.1f       Δ_max = %.1f
fraction Δ > 0 (sound bound):  %.1f %%        n = %s",
             s_min, s_med, s_max, 100 * f_pos,
             format(n, big.mark = ",")),
           hjust = 1, vjust = 1,
           size = 2.2, family = "Helvetica", color = SOFT_INK,
           fontface = "italic", lineheight = 1.35) +

  scale_x_continuous(expand = expansion(mult = c(0.02, 0.02))) +
  scale_y_continuous(expand = expansion(mult = c(0.00, 0.12))) +
  labs(x = expression("Held-out surplus  " *
                       Delta == m %.% log[2] * group("|", Sigma, "|") - "|p|"
                       * "  (bits)"),
       y = "Discoveries") +
  theme_minimal(base_family = "Helvetica") +
  theme(
    panel.grid.minor   = element_blank(),
    panel.grid.major.x = element_line(color = GRID_LINE, linewidth = 0.18),
    panel.grid.major.y = element_line(color = GRID_LINE, linewidth = 0.18),
    axis.title         = element_text(size = 8, color = INK_AXIS),
    axis.text          = element_text(size = 7, color = INK_AXIS),
    axis.line          = element_line(color = INK_AXIS, linewidth = 0.30),
    axis.ticks         = element_line(color = INK_AXIS, linewidth = 0.30),
    axis.ticks.length  = unit(2, "pt"),
    plot.background    = element_rect(fill = PAPER, color = NA),
    panel.background   = element_rect(fill = PAPER, color = NA),
    plot.margin        = margin(8, 10, 6, 8)
  )

# --- Save -----------------------------------------------------------
pdf_path <- file.path(outdir, "fig_gate_surplus.pdf")
png_path <- file.path(outdir, "fig_gate_surplus.png")

# Same envelope as fig6 - keeps intra-paper consistency.
FIG_W_MM <- 114
FIG_H_MM <- 85

ggsave(pdf_path, p, width = FIG_W_MM, height = FIG_H_MM,
       units = "mm", device = cairo_pdf)
ggsave(png_path, p, width = FIG_W_MM, height = FIG_H_MM,
       units = "mm", dpi = 300)

cat(sprintf("\nwrote %s\n      %s\n", pdf_path, png_path))
cat(sprintf("Dimensions: %d mm × %d mm\n", FIG_W_MM, FIG_H_MM))
