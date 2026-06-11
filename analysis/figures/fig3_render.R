#!/usr/bin/env Rscript
# Figure 3 - Resolution-time strip plot, one column per population.
#
# Visualises the "cliff" argument of Section 3.2: discoveries land
# fast and below the budget; everything else piles up against the
# 600 s ceiling. The empty band between the discovery cloud and the
# wall is the structural finding the figure exists to show.
#
# Discovered points take stage colour. All non-discovered points
# (the wall pile-up plus the small set of early-stopping non-finds
# scattered below it) are drawn first/underneath in a muted neutral
# gray at low alpha so the cliff reads at a glance.
#
# Data sources are the four sweep CSVs sitting one directory up. The
# unified table is built in process; no /tmp or cached-CSV dependency.

suppressPackageStartupMessages({
  library(ggplot2)
})

# --- Palette ------------------------------------------------------------
STAGE_COL <- c(
  S1 = "#C28166",
  S2 = "#5DA0CE",
  S3 = "#7B9358",
  S4 = "#1B3556"
)
OTHER_COL    <- "#9F9890"   # muted neutral gray for non-discoveries
PAPER        <- "#FFFFFF"
INK_AXIS     <- "#2A2A2A"
GRID_LINE    <- "#EBE3D4"   # aligned with fig4 / fig6
BUDGET_GREY  <- "#7A6F60"
CEILING_FILL <- "#7A6F60"

DROP_XREF <- c("A000069", "A000120", "A001969", "A002113")

BUDGET_S <- 600
TIME_CAP <- 605             # cap a hair above so wall points sit on/just under the line
Y_MAX    <- 625

STAGE_ORDER <- c("S1", "S2", "S3", "S4")

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
  data.frame(
    id           = raw$id,
    stage        = stage_label,
    class        = raw$solomonoff_class,
    class_simple = ifelse(raw$solomonoff_class == "discovered",
                          "discovered", "other"),
    time_s       = as.numeric(raw$time_s),
    stringsAsFactors = FALSE
  )
}

cat("Loading sweep CSVs...\n")
s1_df <- load_stage(S1_CSV, "S1", apply_dedupe = FALSE)
s2_df <- load_stage(S2_CSV, "S2", apply_dedupe = FALSE)
s3_df <- load_stage(S3_CSV, "S3", apply_dedupe = TRUE)
s4_df <- load_stage(S4_CSV, "S4", apply_dedupe = FALSE)

fig3 <- rbind(s1_df, s2_df, s3_df, s4_df)
fig3$stage <- factor(fig3$stage, levels = STAGE_ORDER)

cat(sprintf("  S1: %d  S2: %d  S3 (deduped): %d  S4: %d\n",
            nrow(s1_df), nrow(s2_df), nrow(s3_df), nrow(s4_df)))

# --- Hard verification gate --------------------------------------------
EXPECTED <- list(
  S1 = list(discovered = 120L,  total = 488L),
  S2 = list(discovered = 244L,  total = 256L),
  S3 = list(discovered = 336L,  total = 983L),
  S4 = list(discovered = 1683L, total = 2187L)
)
EXPECTED_DISC_TOTAL  <- 2383L
EXPECTED_GRAND_TOTAL <- 3914L

cat("\n=== Per-population gate counts ===\n")
cat(sprintf("%-4s  %10s  %10s\n", "pop", "discovered", "total"))
for (stage in STAGE_ORDER) {
  d <- fig3[fig3$stage == stage, ]
  got_disc  <- sum(d$class_simple == "discovered")
  got_total <- nrow(d)
  exp_disc  <- EXPECTED[[stage]]$discovered
  exp_total <- EXPECTED[[stage]]$total
  cat(sprintf("%-4s  %10d  %10d\n", stage, got_disc, got_total))
  if (got_disc != exp_disc) {
    stop(sprintf("%s: discovered = %d, expected %d. Counts do not match the manuscript; aborting before render.",
                 stage, got_disc, exp_disc))
  }
  if (got_total != exp_total) {
    stop(sprintf("%s: total = %d, expected %d.",
                 stage, got_total, exp_total))
  }
}
got_disc_total <- sum(fig3$class_simple == "discovered")
got_grand      <- nrow(fig3)
cat(sprintf("totals: discovered = %d (expected %d), grand = %d (expected %d)\n",
            got_disc_total, EXPECTED_DISC_TOTAL,
            got_grand,      EXPECTED_GRAND_TOTAL))
if (got_disc_total != EXPECTED_DISC_TOTAL) {
  stop(sprintf("discovered total = %d, expected %d.",
               got_disc_total, EXPECTED_DISC_TOTAL))
}
if (got_grand != EXPECTED_GRAND_TOTAL) {
  stop(sprintf("grand total = %d, expected %d.",
               got_grand, EXPECTED_GRAND_TOTAL))
}
cat("Assertion: PASS\n")

# --- Cap time and write side-car --------------------------------------
fig3$time_s <- pmin(fig3$time_s, TIME_CAP)

write.csv(fig3, file.path(here, "fig3_strip_data.csv"), row.names = FALSE)

# --- X-axis labels carry the per-population counts --------------------
STAGE_LABELS <- c(
  S1 = "S1\n120 / 488",
  S2 = "S2\n244 / 256",
  S3 = "S3\n336 / 983",
  S4 = "S4\n1,683 / 2,187"
)

disc_d  <- fig3[fig3$class_simple == "discovered", ]
other_d <- fig3[fig3$class_simple == "other", ]

# --- Background column bands (very faint stage tint) ------------------
band_df <- data.frame(
  stage = STAGE_ORDER,
  xmin  = seq_along(STAGE_ORDER) - 0.5,
  xmax  = seq_along(STAGE_ORDER) + 0.5,
  fill  = unname(STAGE_COL[STAGE_ORDER])
)

# --- Plot --------------------------------------------------------------
jit_disc  <- position_jitter(width = 0.32, height = 0, seed = 42)
jit_other <- position_jitter(width = 0.32, height = 0, seed = 43)

p <- ggplot() +

  # Subtle column tints for the four populations.
  geom_rect(data = band_df,
            aes(xmin = xmin, xmax = xmax,
                ymin = -Inf, ymax = Inf,
                fill = fill),
            alpha = 0.03,
            inherit.aes = FALSE) +

  # Horizontal grid at round numbers (hairline).
  geom_hline(yintercept = c(100, 200, 300, 400, 500),
             color = GRID_LINE, linewidth = 0.2) +

  # Budget ceiling: faint shaded band above 600 s + a single tenuous
  # solid hairline at 600 s. Solid (not dashed) so it renders
  # consistently across PDF viewers at small sizes, and light enough
  # that it cedes the foreground to the dots.
  annotate("rect",
           xmin = 0.5, xmax = length(STAGE_ORDER) + 0.5,
           ymin = BUDGET_S, ymax = Y_MAX,
           fill = CEILING_FILL, alpha = 0.05) +
  geom_hline(yintercept = BUDGET_S,
             color = "#C8C0B4", linetype = "solid",
             linewidth = 0.18) +
  annotate("text",
           x = length(STAGE_ORDER) + 0.48, y = BUDGET_S,
           label = "budget = 600 s",
           color = BUDGET_GREY,
           hjust = 1, vjust = -0.5,
           size = 2.1, fontface = "italic", family = "Helvetica") +

  # 1) Non-discoveries first/underneath - muted gray, low alpha.
  #    Vast majority sit at the wall; a few early-stop below it.
  geom_point(data = other_d,
             aes(x = stage, y = time_s),
             shape = 21,
             fill = OTHER_COL, color = "white",
             alpha = 0.25, size = 0.45, stroke = 0.10,
             position = jit_other) +

  # 2) Discoveries on top - stage colour, more saturated.
  geom_point(data = disc_d,
             aes(x = stage, y = time_s, fill = stage),
             shape = 21, color = "white",
             alpha = 0.75, size = 0.65, stroke = 0.10,
             position = jit_disc) +

  scale_fill_manual(values = c(STAGE_COL,
                               setNames(unname(STAGE_COL),
                                        unname(STAGE_COL))),
                    guide = "none") +
  scale_x_discrete(limits = STAGE_ORDER, labels = STAGE_LABELS,
                   expand = expansion(add = 0.5)) +
  scale_y_continuous(breaks = c(0, 100, 200, 300, 400, 500, 600),
                     limits = c(0, Y_MAX),
                     expand = c(0, 0)) +
  coord_cartesian(clip = "off") +
  labs(x = NULL, y = "Resolution time (s)") +
  theme_minimal(base_family = "Helvetica") +
  theme(
    panel.grid         = element_blank(),
    panel.border       = element_blank(),
    axis.title.y       = element_text(size = 8, color = INK_AXIS),
    axis.text.y        = element_text(size = 7, color = INK_AXIS),
    axis.text.x        = element_text(size = 7, color = INK_AXIS,
                                      lineheight = 1.0),
    axis.line.x        = element_line(color = INK_AXIS, linewidth = 0.35),
    axis.line.y        = element_line(color = INK_AXIS, linewidth = 0.35),
    axis.ticks.y       = element_line(color = INK_AXIS, linewidth = 0.35),
    axis.ticks.x       = element_blank(),
    axis.ticks.length  = unit(2.5, "pt"),
    plot.background    = element_rect(fill = PAPER, color = NA),
    panel.background   = element_rect(fill = PAPER, color = NA),
    plot.margin        = margin(8, 10, 6, 8)
  )

# --- Save --------------------------------------------------------------
# Single-column PNAS width (87 mm); naturally portrait (0-625 on y).
pdf_path <- file.path(here, "fig3_strip.pdf")
png_path <- file.path(here, "fig3_strip.png")

FIG_W_MM <- 87
FIG_H_MM <- 140

ggsave(pdf_path, p, width = FIG_W_MM, height = FIG_H_MM,
       units = "mm", device = cairo_pdf)
ggsave(png_path, p, width = FIG_W_MM, height = FIG_H_MM,
       units = "mm", dpi = 300)

# --- Clean up obsolete _v2 outputs (if present) ----------------------
for (old in c("fig3_strip_v2.pdf", "fig3_strip_v2.png", "fig3_data.csv")) {
  pth <- file.path(here, old)
  if (file.exists(pth)) {
    file.remove(pth)
    cat(sprintf("removed obsolete artifact: %s\n", old))
  }
}

cat(sprintf("\nwrote %s\n      %s\n      fig3_strip_data.csv\n",
            pdf_path, png_path))
cat(sprintf("Dimensions: %d mm x %d mm  (single-column, portrait)\n",
            FIG_W_MM, FIG_H_MM))
