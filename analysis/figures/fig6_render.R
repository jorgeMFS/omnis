#!/usr/bin/env Rscript
# Figure 6 - The Compression-Prediction Landscape.
#
# Single panel, full four-population corpus:
#   S1 (mixed OEIS) + S2 (elementary CA) + S3-deduped (OEIS base+core)
#   + S4 (totalistic 3-state CA)  =  3,914 sequences.
#
# The figure's load-bearing classes are the 17 compressed-only and
# 27 predicted-only points - the off-diagonal counts quoted directly
# in the paper text. Drawing order and styling are tuned so those
# two small classes stay legible against the dense (~1,500-point)
# `neither` background.
#
# Z-order, back to front:
#   neither     - light gray, low alpha, small         (background)
#   discovered  - blue,  medium alpha, medium          (diagonal band)
#   predicted_only \  full opacity, larger, thin dark  (off-diagonal,
#   compressed_only /  stroke around each marker        story-relevant)
#
# Data sources are the four sweep CSVs sitting one directory up. The
# unified table is built in process; no /tmp dependency.

suppressPackageStartupMessages({
  library(ggplot2)
})

# --- Palette ------------------------------------------------------------
CLASS_COL <- c(
  discovered               = "#5DA0CE",
  compressed_only          = "#E8A838",
  not_compressed_predicted = "#D14F2D",
  neither                  = "#C8C0B4"  # lighter than before — recedes
)

BG          <- "#FFFFFF"
INK_AXIS    <- "#2A2A2A"
GRID_LINE   <- "#EBE3D4"
GUIDE_GREY  <- "#7A6F60"
DIAG_GREY   <- "#C4BBA9"  # y = x reference, subordinate to the gates
STROKE_INK  <- "#2A2A2A"

DROP_XREF <- c("A000069", "A000120", "A001969", "A002113")

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
load_stage <- function(csv_path, stage_label, apply_dedupe = FALSE) {
  raw <- read.csv(csv_path, stringsAsFactors = FALSE)
  if (apply_dedupe) {
    raw <- raw[!(raw$category == "oeis_base" &
                 raw$oeis_xref %in% DROP_XREF), ]
  }
  A          <- as.numeric(raw$A)
  train_bits <- as.numeric(raw$train_n) * log2(A)
  mdl        <- as.numeric(raw$mdl)
  data.frame(
    id          = raw$id,
    population  = stage_label,
    class       = raw$solomonoff_class,
    train_bits  = train_bits,
    mdl         = mdl,
    train_ratio = ifelse(train_bits == 0, NA_real_, mdl / train_bits),
    total_ratio = as.numeric(raw$ratio),
    stringsAsFactors = FALSE
  )
}

cat("Loading sweep CSVs...\n")
s1_df <- load_stage(S1_CSV, "S1", apply_dedupe = FALSE)
s2_df <- load_stage(S2_CSV, "S2", apply_dedupe = FALSE)
s3_df <- load_stage(S3_CSV, "S3", apply_dedupe = TRUE)
s4_df <- load_stage(S4_CSV, "S4", apply_dedupe = FALSE)

fig6 <- rbind(s1_df, s2_df, s3_df, s4_df)

cat(sprintf("  S1: %d  S2: %d  S3 (deduped): %d  S4: %d\n",
            nrow(s1_df), nrow(s2_df), nrow(s3_df), nrow(s4_df)))
cat(sprintf("  Full corpus: %d rows\n", nrow(fig6)))

# --- Hard verification gate --------------------------------------------
EXPECTED <- c(discovered = 2383L,
              compressed_only = 17L,
              not_compressed_predicted = 27L,
              neither = 1487L)

cls_levels <- names(EXPECTED)
cnt <- table(factor(fig6$class, levels = cls_levels))
cat(sprintf("\n=== Full-corpus class counts (n = %d) ===\n", sum(cnt)))
print(cnt)
for (cls in cls_levels) {
  got <- as.integer(cnt[[cls]])
  if (got != as.integer(EXPECTED[[cls]])) {
    stop(sprintf("Class '%s' = %d, expected %d. Counts do not match the manuscript; aborting before render.",
                 cls, got, EXPECTED[[cls]]))
  }
}
if (sum(cnt) != sum(EXPECTED)) {
  stop(sprintf("Total = %d, expected %d.", sum(cnt), sum(EXPECTED)))
}
cat("Assertion: PASS\n")

# Pull the load-bearing counts straight from the asserted table so the
# on-plot annotations can never drift from the data.
N_PRED <- as.integer(cnt[["not_compressed_predicted"]])
N_COMP <- as.integer(cnt[["compressed_only"]])
N_DISC <- as.integer(cnt[["discovered"]])
N_NEIT <- as.integer(cnt[["neither"]])

# --- Single side-car ---------------------------------------------------
write.csv(fig6, file.path(here, "fig6_data.csv"), row.names = FALSE)

# --- Class subsets for layered drawing ---------------------------------
d_neither <- fig6[fig6$class == "neither", ]
d_disc    <- fig6[fig6$class == "discovered", ]
d_pred    <- fig6[fig6$class == "not_compressed_predicted", ]
d_comp    <- fig6[fig6$class == "compressed_only", ]

# --- Axis range (shared with previous fig6 conventions) ---------------
# y widened to 1.3 to keep the lone predicted-only point at total_ratio
# = 1.24 (A056964_a2) on-plot. Clipped points beyond these limits are
# all in the `neither` long tail.
XLIM <- c(0, 3.0)
YLIM <- c(0, 1.3)

# --- Legend labels driven by asserted counts --------------------------
legend_labels <- c(
  discovered               = sprintf("discovered (%s)",
                                     format(N_DISC, big.mark = ",")),
  compressed_only          = sprintf("compressed only (%d)", N_COMP),
  not_compressed_predicted = sprintf("predicted only (%d)", N_PRED),
  neither                  = sprintf("neither (%s)",
                                     format(N_NEIT, big.mark = ","))
)

# --- Plot --------------------------------------------------------------
p <- ggplot() +

  # Faint y = x guide (subordinate to the two boundary gates).
  geom_segment(aes(x = 0, y = 0, xend = 1.3, yend = 1.3),
               linetype = "dotted", color = DIAG_GREY,
               linewidth = 0.25) +

  # The two boundary gates (dashed, slightly heavier).
  geom_vline(xintercept = 1.0, linetype = "dashed",
             color = GUIDE_GREY, linewidth = 0.3) +
  geom_hline(yintercept = 1.0, linetype = "dashed",
             color = GUIDE_GREY, linewidth = 0.3) +

  # --- Z-order, back to front ---
  #
  # 1) `neither` - light gray cloud, recedes.
  geom_point(data = d_neither,
             aes(x = train_ratio, y = total_ratio, color = class),
             alpha = 0.20, size = 0.45, na.rm = TRUE) +
  # 2) `discovered` - the diagonal band.
  geom_point(data = d_disc,
             aes(x = train_ratio, y = total_ratio, color = class),
             alpha = 0.55, size = 0.95, na.rm = TRUE) +
  # 3+4) Off-diagonal classes drawn last as halo + marker so the
  #      17 + 27 story-relevant points stay crisp against the cloud.
  #      Halo is a slightly larger dark dot underneath, producing
  #      a thin dark ring around each colored marker.
  geom_point(data = d_pred,
             aes(x = train_ratio, y = total_ratio),
             color = STROKE_INK, size = 2.05, alpha = 1, na.rm = TRUE) +
  geom_point(data = d_pred,
             aes(x = train_ratio, y = total_ratio, color = class),
             size = 1.65, alpha = 1, na.rm = TRUE) +
  geom_point(data = d_comp,
             aes(x = train_ratio, y = total_ratio),
             color = STROKE_INK, size = 2.05, alpha = 1, na.rm = TRUE) +
  geom_point(data = d_comp,
             aes(x = train_ratio, y = total_ratio, color = class),
             size = 1.65, alpha = 1, na.rm = TRUE) +

  # Inline labels for the two gates.
  annotate("text", x = 1.0, y = 0.25,
           label = "mdl = train_bits", color = GUIDE_GREY,
           angle = 90, hjust = 0.5, vjust = -0.6,
           size = 2.2, fontface = "italic", family = "Helvetica") +
  annotate("text", x = 2.55, y = 1.0,
           label = "mdl = raw_bits", color = GUIDE_GREY,
           hjust = 0.5, vjust = -0.6,
           size = 2.2, fontface = "italic", family = "Helvetica") +

  # On-plot annotations for the two off-diagonal regions.
  # Counts come from the asserted count table (N_PRED, N_COMP) - they
  # cannot drift from the data even if the sweep is rerun.
  annotate("text",
           x = 2.05, y = 0.55,
           label = sprintf("predicted only (%d)", N_PRED),
           color = "#7E2A18", hjust = 0, vjust = 0.5,
           size = 2.3, fontface = "italic", family = "Helvetica") +
  annotate("text",
           x = 0.45, y = 0.70,
           label = sprintf("compressed only (%d)", N_COMP),
           color = "#7A5A0F", hjust = 0, vjust = 0.5,
           size = 2.3, fontface = "italic", family = "Helvetica") +

  # All four classes participate in the color scale (the off-diagonal
  # halo is unmapped, so it does not enter the legend). One combined
  # legend, in stage-color order.
  scale_color_manual(values = CLASS_COL,
                     breaks = c("discovered",
                                "compressed_only",
                                "not_compressed_predicted",
                                "neither"),
                     labels = legend_labels,
                     name = NULL,
                     guide = guide_legend(
                       nrow = 2, byrow = TRUE,
                       override.aes = list(size = 1.8, alpha = 1)
                     )) +
  scale_x_continuous(breaks = c(0, 0.5, 1, 1.5, 2, 2.5, 3),
                     expand = c(0.01, 0)) +
  scale_y_continuous(breaks = c(0, 0.25, 0.5, 0.75, 1.0, 1.25),
                     expand = c(0.01, 0)) +
  coord_cartesian(xlim = XLIM, ylim = YLIM, clip = "on") +
  labs(x = "Compression ratio (mdl / train bits)",
       y = "Total ratio (mdl / raw bits)") +
  theme_minimal(base_family = "Helvetica") +
  theme(
    panel.grid.minor   = element_blank(),
    panel.grid.major   = element_line(color = GRID_LINE, linewidth = 0.18),
    axis.title         = element_text(size = 8, color = INK_AXIS),
    axis.text          = element_text(size = 7, color = INK_AXIS),
    axis.line          = element_line(color = INK_AXIS, linewidth = 0.3),
    axis.ticks         = element_line(color = INK_AXIS, linewidth = 0.3),
    axis.ticks.length  = unit(2, "pt"),
    legend.position    = "bottom",
    legend.box         = "horizontal",
    legend.text        = element_text(size = 7, color = INK_AXIS,
                                      family = "Helvetica"),
    legend.key         = element_rect(fill = NA, color = NA),
    legend.key.size    = unit(3.0, "mm"),
    legend.spacing.x   = unit(4, "pt"),
    legend.margin      = margin(2, 0, 0, 0),
    plot.background    = element_rect(fill = BG, color = NA),
    panel.background   = element_rect(fill = BG, color = NA),
    plot.margin        = margin(8, 10, 4, 8)
  )

# --- Save --------------------------------------------------------------
# 1.5-column PNAS width - ~3,900 points would feel cramped at
# single-column (87 mm), and the two small off-diagonal classes need
# room to read. Gentle near-square aspect so the unit box has vertical
# presence rather than being squished into a wide band.
pdf_path <- file.path(here, "fig6_landscape.pdf")
png_path <- file.path(here, "fig6_landscape.png")

FIG_W_MM <- 114
FIG_H_MM <- 112   # +7 mm to host the wrapped two-row legend cleanly

ggsave(pdf_path, p, width = FIG_W_MM, height = FIG_H_MM,
       units = "mm", device = cairo_pdf)
ggsave(png_path, p, width = FIG_W_MM, height = FIG_H_MM,
       units = "mm", dpi = 300)

# --- Clean up obsolete two-panel side-cars (if present) ---------------
for (old in c("fig6_data_panelA.csv", "fig6_data_panelB.csv")) {
  pth <- file.path(here, old)
  if (file.exists(pth)) {
    file.remove(pth)
    cat(sprintf("removed obsolete side-car: %s\n", old))
  }
}

cat(sprintf("\nwrote %s\n      %s\n      fig6_data.csv\n",
            pdf_path, png_path))
cat(sprintf("Dimensions: %d mm x %d mm  (1.5-column, near-square)\n",
            FIG_W_MM, FIG_H_MM))
