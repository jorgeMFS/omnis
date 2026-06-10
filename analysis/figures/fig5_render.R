#!/usr/bin/env Rscript
# Figure 5 — The ECA grid.
# 16 x 16 cells, one per elementary-CA rule (0..255). Rule 0 sits at
# top-left, rule 255 at bottom-right, row-major. Cells are coloured
# by Solomonoff class. Rules 30 and 110 carry a thin dark outline and
# bold rule number — quiet highlights, not stamps.

suppressPackageStartupMessages({
  library(ggplot2)
})

# --- Palette ------------------------------------------------------------
CLASS_COL <- c(
  discovered      = "#5DA0CE",
  compressed_only = "#E8A838",
  neither         = "#B8B0A4"
)
CLASS_TEXT <- c(
  discovered      = "#FFFFFF",
  compressed_only = "#332515",
  neither         = "#2F2A22"
)

BG       <- "#FFFFFF"
INK_AXIS <- "#2A2A2A"
HILITE   <- "#2A2A2A"

# --- Resolve paths ------------------------------------------------------
script_path <- tryCatch({
  args <- commandArgs(trailingOnly = FALSE)
  fa <- args[grep("^--file=", args)]
  if (length(fa)) normalizePath(sub("^--file=", "", fa[1])) else NA_character_
}, error = function(e) NA_character_)
here    <- if (is.na(script_path)) getwd() else dirname(script_path)
csv_dir <- normalizePath(file.path(here, "..", "..", "data", "results"))
csv_in  <- file.path(csv_dir, "baseline_20260513T232442Z.csv")

# --- Load + parse rule_number -----------------------------------------
raw <- read.csv(csv_in, stringsAsFactors = FALSE)
fig5 <- data.frame(
  rule_number = as.integer(sub("^eca_", "", raw$id)),
  class       = raw$solomonoff_class,
  stringsAsFactors = FALSE
)
fig5 <- fig5[order(fig5$rule_number), ]
stopifnot(nrow(fig5) == 256, all(fig5$rule_number == 0:255))

write.csv(fig5, file.path(here, "fig5_data.csv"), row.names = FALSE)

# --- Counts + verification --------------------------------------------
cnt <- table(factor(fig5$class,
                    levels = c("discovered", "compressed_only", "neither")))
cat("\n=== Class counts ===\n"); print(cnt)
for (cls in c("compressed_only", "neither")) {
  rules <- fig5$rule_number[fig5$class == cls]
  cat(sprintf("\n%s (%d rules): %s\n",
              cls, length(rules), paste(sort(rules), collapse = ", ")))
}
cat("\n")

# --- Layout coordinates ------------------------------------------------
fig5$col <- fig5$rule_number %% 16
fig5$row <- fig5$rule_number %/% 16
fig5$text_col <- CLASS_TEXT[fig5$class]

HI_RULES   <- c(30, 110)
hi         <- fig5[fig5$rule_number %in% HI_RULES, ]
regular_d  <- fig5[!(fig5$rule_number %in% HI_RULES), ]

legend_labels <- c(
  discovered      = sprintf("discovered (%d)", cnt[["discovered"]]),
  compressed_only = sprintf("compressed only (%d)", cnt[["compressed_only"]]),
  neither         = sprintf("neither (%d)", cnt[["neither"]])
)

# --- Plot --------------------------------------------------------------
p <- ggplot(fig5, aes(x = col, y = row)) +
  # Cells, with a slim white gutter for breathing room.
  geom_tile(aes(fill = class),
            color = BG, linewidth = 0.7) +
  # Regular cells: rule number in subtle text.
  geom_text(data = regular_d,
            aes(label = rule_number, color = text_col),
            size = 1.5, family = "Helvetica",
            inherit.aes = TRUE) +
  # Highlighted cells (30, 110): thin dark outline...
  geom_tile(data = hi,
            aes(x = col, y = row),
            fill = NA, color = HILITE, linewidth = 0.45,
            width = 1, height = 1,
            inherit.aes = FALSE) +
  # ...and a bold rule number inside.
  geom_text(data = hi,
            aes(x = col, y = row,
                label = rule_number, color = text_col),
            size = 1.8, fontface = "bold", family = "Helvetica",
            inherit.aes = FALSE) +
  scale_fill_manual(values = CLASS_COL,
                    breaks = c("discovered",
                               "compressed_only",
                               "neither"),
                    labels = legend_labels,
                    name = NULL,
                    guide = guide_legend(
                      override.aes = list(color = NA),
                      keywidth  = unit(2.8, "mm"),
                      keyheight = unit(2.8, "mm")
                    )) +
  scale_color_identity() +
  scale_y_reverse(expand = c(0, 0)) +
  scale_x_continuous(expand = c(0, 0)) +
  coord_equal(clip = "off") +
  theme_minimal(base_family = "Helvetica") +
  theme(
    panel.grid       = element_blank(),
    axis.title       = element_blank(),
    axis.text        = element_blank(),
    axis.ticks       = element_blank(),
    legend.position  = "bottom",
    legend.box       = "horizontal",
    legend.title     = element_blank(),
    legend.text      = element_text(size = 6, color = INK_AXIS,
                                    family = "Helvetica"),
    legend.key       = element_rect(fill = NA, color = NA),
    legend.spacing.x = unit(6, "pt"),
    legend.margin    = margin(4, 0, 0, 0),
    plot.background  = element_rect(fill = BG, color = NA),
    panel.background = element_rect(fill = BG, color = NA),
    plot.margin      = margin(6, 8, 4, 8)
  )

# --- Save --------------------------------------------------------------
pdf_path <- file.path(here, "fig5_eca_grid.pdf")
png_path <- file.path(here, "fig5_eca_grid.png")
ggsave(pdf_path, p, width = 89, height = 95, units = "mm",
       device = cairo_pdf)
ggsave(png_path, p, width = 89, height = 95, units = "mm",
       dpi = 300)

cat(sprintf("wrote %s\n%s\n%s\n",
            file.path(here, "fig5_data.csv"), pdf_path, png_path))
