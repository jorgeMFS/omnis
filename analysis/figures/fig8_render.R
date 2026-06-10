#!/usr/bin/env Rscript
# Figure 8 — S4 discoverability by rule structure.
# Each totalistic 3-state CA rule is a 7-digit base-3 number
# (d0 d1 d2 d3 d4 d5 d6), where d_i is the output when the
# neighborhood-state sum equals i. We bin rules by
#   low_sum  = d0 + d1 + d2   (response to sparse neighborhoods)
#   high_sum = d4 + d5 + d6   (response to dense neighborhoods)
# and colour each bin by the local discovery rate. d3 (middle) is
# marginalised. Cells with no rules are left blank.

suppressPackageStartupMessages({
  library(ggplot2)
})

# --- Palette ------------------------------------------------------------
LOW_COL  <- "#EEEAE0"   # near-empty bin colour (0 % discovered)
HIGH_COL <- "#1B3556"   # S4 stage colour (100 % discovered)
BG       <- "#FFFFFF"
INK_AXIS <- "#2A2A2A"

# --- Resolve paths ------------------------------------------------------
script_path <- tryCatch({
  args <- commandArgs(trailingOnly = FALSE)
  fa <- args[grep("^--file=", args)]
  if (length(fa)) normalizePath(sub("^--file=", "", fa[1])) else NA_character_
}, error = function(e) NA_character_)
here    <- if (is.na(script_path)) getwd() else dirname(script_path)
csv_dir <- normalizePath(file.path(here, "..", "..", "data", "results"))
csv_in  <- file.path(csv_dir, "baseline_20260520T155701Z.csv")

# --- Load + parse ------------------------------------------------------
raw <- read.csv(csv_in, stringsAsFactors = FALSE)
fig8 <- data.frame(
  rule_number = as.integer(sub("^tot3_", "", raw$id)),
  class       = raw$solomonoff_class,
  stringsAsFactors = FALSE
)
fig8 <- fig8[order(fig8$rule_number), ]
stopifnot(nrow(fig8) == 2187)

# --- Base-3 decomposition ---------------------------------------------
base3_digits <- function(n) {
  d <- integer(7)
  n <- as.integer(n)
  for (i in seq_len(7)) {
    d[i] <- as.integer(n %% 3L)
    n    <- as.integer(n %/% 3L)
  }
  d
}
digit_mat <- t(vapply(fig8$rule_number, base3_digits,
                      FUN.VALUE = integer(7)))
colnames(digit_mat) <- paste0("d", 0:6)
fig8 <- cbind(fig8, digit_mat)

fig8$low_sum  <- fig8$d0 + fig8$d1 + fig8$d2
fig8$high_sum <- fig8$d4 + fig8$d5 + fig8$d6

# --- Aggregate per (low_sum, high_sum) bin ----------------------------
agg <- aggregate(class ~ low_sum + high_sum, data = fig8,
                 FUN = function(x) c(n      = length(x),
                                     n_disc = sum(x == "discovered"),
                                     n_neit = sum(x == "neither"),
                                     n_comp = sum(x == "compressed_only")))
agg <- do.call(data.frame, agg)
names(agg) <- c("low_sum", "high_sum",
                "n", "n_disc", "n_neit", "n_comp")
agg$disc_rate <- agg$n_disc / agg$n

write.csv(agg, file.path(here, "fig8_data.csv"), row.names = FALSE)

cat("\n=== Bin counts (low_sum, high_sum) ===\n")
print(agg[order(agg$low_sum, agg$high_sum), ], row.names = FALSE)

cat(sprintf("\nGlobal discovery rate: %.1f%%  (1683 / 2187)\n",
            100 * 1683 / 2187))

# --- Plot --------------------------------------------------------------
agg$label <- sprintf("%.0f%%", 100 * agg$disc_rate)

p <- ggplot(agg, aes(x = low_sum, y = high_sum)) +
  geom_tile(aes(fill = disc_rate),
            color = BG, linewidth = 0.7) +
  geom_text(aes(label = label,
                color = ifelse(disc_rate > 0.55, "#FFFFFF", INK_AXIS)),
            size = 2.2, family = "Helvetica") +
  scale_color_identity() +
  scale_fill_gradient(low = LOW_COL, high = HIGH_COL,
                      limits = c(0, 1),
                      breaks = c(0, 0.5, 1),
                      labels = c("0%", "50%", "100%"),
                      name = "discovery rate") +
  scale_x_continuous(breaks = 0:6, expand = c(0, 0)) +
  scale_y_continuous(breaks = 0:6, expand = c(0, 0)) +
  coord_equal(clip = "off") +
  labs(x = expression("Low-density output mass  " * (d[0] + d[1] + d[2])),
       y = expression("High-density output mass  " * (d[4] + d[5] + d[6]))) +
  guides(fill = guide_colorbar(barwidth  = unit(35, "mm"),
                               barheight = unit(2,  "mm"),
                               ticks.colour = INK_AXIS,
                               frame.colour = NA,
                               title.position = "left",
                               title.vjust = 0.9)) +
  theme_minimal(base_family = "Helvetica") +
  theme(
    panel.grid       = element_blank(),
    axis.title       = element_text(size = 7.5, color = INK_AXIS),
    axis.text        = element_text(size = 6.5, color = INK_AXIS),
    axis.line        = element_line(color = INK_AXIS, linewidth = 0.3),
    axis.ticks       = element_line(color = INK_AXIS, linewidth = 0.3),
    axis.ticks.length = unit(2, "pt"),
    legend.position  = "bottom",
    legend.title     = element_text(size = 6.5, color = INK_AXIS,
                                    family = "Helvetica"),
    legend.text      = element_text(size = 6, color = INK_AXIS,
                                    family = "Helvetica"),
    legend.margin    = margin(4, 0, 0, 0),
    plot.background  = element_rect(fill = BG, color = NA),
    panel.background = element_rect(fill = BG, color = NA),
    plot.margin      = margin(6, 8, 4, 8)
  )

# --- Save --------------------------------------------------------------
pdf_path <- file.path(here, "fig8_s4_structure.pdf")
png_path <- file.path(here, "fig8_s4_structure.png")
ggsave(pdf_path, p, width = 100, height = 110, units = "mm",
       device = cairo_pdf)
ggsave(png_path, p, width = 100, height = 110, units = "mm",
       dpi = 300)

cat(sprintf("\nwrote %s\n%s\n%s\n",
            file.path(here, "fig8_data.csv"), pdf_path, png_path))
