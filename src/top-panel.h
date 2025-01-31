/*
 * Copyright (C) 2018 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "layersurface.h"

#define PHOSH_TYPE_TOP_PANEL            (phosh_top_panel_get_type ())

G_DECLARE_FINAL_TYPE (PhoshTopPanel, phosh_top_panel, PHOSH, TOP_PANEL, PhoshLayerSurface)

#define PHOSH_TOP_PANEL_HEIGHT 32

/**
 * PhoshTopPanelState:
 * @PHOSH_TOP_PANEL_STATE_FOLDED: Only top-bar is visible
 * @PHOSH_TOP_PANEL_STATE_UNFOLDED: Settings menu is unfolded
 */
typedef enum {
  PHOSH_TOP_PANEL_STATE_FOLDED,
  PHOSH_TOP_PANEL_STATE_UNFOLDED,
} PhoshTopPanelState;

GtkWidget         *phosh_top_panel_new (struct zwlr_layer_shell_v1 *layer_shell,
                                        struct wl_output           *wl_output);
void               phosh_top_panel_toggle_fold (PhoshTopPanel *self);
void               phosh_top_panel_fold (PhoshTopPanel *self);
void               phosh_top_panel_unfold (PhoshTopPanel *self);
PhoshTopPanelState phosh_top_panel_get_state (PhoshTopPanel *self);
