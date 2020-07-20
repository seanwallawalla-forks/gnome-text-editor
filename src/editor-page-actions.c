/* editor-page-actions.c
 *
 * Copyright 2020 Christian Hergert <chergert@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "editor-page-actions"

#include "editor-page-private.h"
#include "editor-language-dialog.h"

static void
editor_page_actions_language (GSimpleAction *action,
                              GVariant      *param,
                              gpointer       user_data)
{
  EditorPage *self = user_data;
  EditorLanguageDialog *dialog;

  g_assert (EDITOR_IS_PAGE (self));

  dialog = editor_language_dialog_new (NULL);
  g_object_bind_property (dialog, "language",
                          self, "language",
                          (G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL));
  gtk_window_present (GTK_WINDOW (dialog));
}

static void
editor_page_actions_search_hide (GSimpleAction *action,
                                 GVariant      *param,
                                 gpointer       user_data)
{
  EditorPage *self = user_data;

  g_assert (EDITOR_IS_PAGE (self));

  _editor_page_hide_search (self);
  gtk_widget_grab_focus (GTK_WIDGET (self));
}

void
_editor_page_class_actions_init (EditorPageClass *klass)
{
}

void
_editor_page_actions_init (EditorPage *self)
{
  static const GActionEntry page_actions[] = {
    { "language", editor_page_actions_language, "s" },
  };
  static const GActionEntry search_actions[] = {
    { "hide", editor_page_actions_search_hide },
  };

  g_autoptr(GSimpleActionGroup) page = g_simple_action_group_new ();
  g_autoptr(GSimpleActionGroup) search = g_simple_action_group_new ();

  g_action_map_add_action_entries (G_ACTION_MAP (page), page_actions, G_N_ELEMENTS (page_actions), self);
  g_action_map_add_action_entries (G_ACTION_MAP (search), search_actions, G_N_ELEMENTS (search_actions), self);

  gtk_widget_insert_action_group (GTK_WIDGET (self), "page", G_ACTION_GROUP (page));
  gtk_widget_insert_action_group (GTK_WIDGET (self), "search", G_ACTION_GROUP (search));
}
