/* editor-session.h
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

#pragma once

#include "editor-types.h"

G_BEGIN_DECLS

#define EDITOR_TYPE_SESSION (editor_session_get_type())

G_DECLARE_FINAL_TYPE (EditorSession, editor_session, EDITOR, SESSION, GObject)

void          editor_session_add_window         (EditorSession        *self,
                                                 EditorWindow         *window);
EditorWindow *editor_session_create_window      (EditorSession        *self);
void          editor_session_open               (EditorSession        *self,
                                                 EditorWindow         *window,
                                                 GFile                *file);
void          editor_session_open_files         (EditorSession        *self,
                                                 GFile               **files,
                                                 gint                  n_files);
void          editor_session_add_page           (EditorSession        *self,
                                                 EditorWindow         *window,
                                                 EditorPage           *page);
void          editor_session_add_document       (EditorSession        *self,
                                                 EditorWindow         *window,
                                                 EditorDocument       *document);
void          editor_session_add_draft          (EditorSession        *self,
                                                 EditorWindow         *window);
void          editor_session_remove_page        (EditorSession        *self,
                                                 EditorPage           *page);
void          editor_session_remove_document    (EditorSession        *self,
                                                 EditorDocument       *document);
void          editor_session_remove_window      (EditorSession        *self,
                                                 EditorWindow         *window);
void          editor_session_restore_async      (EditorSession        *self,
                                                 GCancellable         *cancellable,
                                                 GAsyncReadyCallback   callback,
                                                 gpointer              user_data);
gboolean      editor_session_restore_finish     (EditorSession        *self,
                                                 GAsyncResult         *result,
                                                 GError              **error);
void          editor_session_save_async         (EditorSession        *self,
                                                 GCancellable         *cancellable,
                                                 GAsyncReadyCallback   callback,
                                                 gpointer              user_data);
gboolean      editor_session_save_finish        (EditorSession        *self,
                                                 GAsyncResult         *result,
                                                 GError              **error);
void          editor_session_load_recent_async  (EditorSession        *self,
                                                 GCancellable         *cancellable,
                                                 GAsyncReadyCallback   callback,
                                                 gpointer              user_data);
GPtrArray    *editor_session_load_recent_finish (EditorSession        *self,
                                                 GAsyncResult         *result,
                                                 GError              **error);

G_END_DECLS
