/* editor-text-buffer-spell-adapter.c
 *
 * Copyright 2021 Christian Hergert <chergert@redhat.com>
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

#include "config.h"

#include "cjhtextregionprivate.h"

#include "editor-spell-checker.h"
#include "editor-spell-cursor.h"
#include "editor-text-buffer-spell-adapter.h"

#define UNCHECKED          GSIZE_TO_POINTER(0)
#define CHECKED            GSIZE_TO_POINTER(1)
#define UPDATE_DELAY_MSECS 200

typedef struct
{
  gint64   deadline;
  guint    has_unchecked : 1;
} Update;

typedef struct
{
  gsize offset;
  guint found : 1;
} ScanForUnchecked;

struct _EditorTextBufferSpellAdapter
{
  GObject             parent_instance;

  GtkTextBuffer      *buffer;
  EditorSpellChecker *checker;
  CjhTextRegion      *region;
  GtkTextTag         *tag;
  GtkTextTag         *no_spell_check_tag;

  guint               cursor_position;

  guint               update_source;

  guint               enabled : 1;
};

G_DEFINE_TYPE (EditorTextBufferSpellAdapter, editor_text_buffer_spell_adapter, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_BUFFER,
  PROP_CHECKER,
  N_PROPS
};

static GParamSpec *properties [N_PROPS];

EditorTextBufferSpellAdapter *
editor_text_buffer_spell_adapter_new (GtkTextBuffer      *buffer,
                                      EditorSpellChecker *checker)
{
  g_return_val_if_fail (GTK_IS_TEXT_BUFFER (buffer), NULL);
  g_return_val_if_fail (!checker || EDITOR_IS_SPELL_CHECKER (checker), NULL);

  return g_object_new (EDITOR_TYPE_TEXT_BUFFER_SPELL_ADAPTER,
                       "buffer", buffer,
                       "checker", checker,
                       NULL);
}

static gboolean
scan_for_next_unchecked_cb (gsize                   offset,
                            const CjhTextRegionRun *run,
                            gpointer                user_data)
{
  ScanForUnchecked *state = user_data;

  if (run->data == UNCHECKED)
    {
      state->offset = offset;
      state->found = TRUE;
      return TRUE;
    }

  return FALSE;
}

static gboolean
scan_for_next_unchecked (CjhTextRegion *region,
                         gsize          begin,
                         gsize          end,
                         gsize         *position)
{
  ScanForUnchecked state = {0};
  _cjh_text_region_foreach_in_range (region, begin, end, scan_for_next_unchecked_cb, &state);
  *position = state.offset;
  return state.found;
}

static gboolean
editor_text_buffer_spell_adapter_update_range (EditorTextBufferSpellAdapter *self,
                                               gsize                         begin_offset,
                                               gsize                         end_offset,
                                               gint64                        deadline)
{
  EditorSpellCursor cursor;
  GtkTextIter begin, end;
  gsize position;
  char *word;

  g_assert (EDITOR_IS_TEXT_BUFFER_SPELL_ADAPTER (self));

  if (begin_offset == end_offset)
    return FALSE;

  if (!scan_for_next_unchecked (self->region, begin_offset, end_offset, &position))
    return FALSE;

  gtk_text_buffer_get_iter_at_offset (self->buffer, &begin, position);
  gtk_text_buffer_get_iter_at_offset (self->buffer, &end, end_offset);
  gtk_text_buffer_remove_tag (self->buffer, self->tag, &begin, &end);

  editor_spell_cursor_init (&cursor, &begin, &end, self->tag);
  while ((word = editor_spell_cursor_next_word (&cursor)))
    {
      if (!editor_spell_cursor_contains_tag (&cursor, self->no_spell_check_tag))
        {
          if (!editor_spell_checker_check_word (self->checker, word, -1))
            editor_spell_cursor_tag (&cursor);
        }

      g_free (word);
    }

  _cjh_text_region_replace (self->region, begin_offset, end_offset - begin_offset, CHECKED);

  return FALSE;
}

static gboolean
editor_text_buffer_spell_adapter_update (EditorTextBufferSpellAdapter *self)
{
  gint64 deadline;
  gboolean has_more;
  gsize length;

  g_assert (EDITOR_IS_TEXT_BUFFER_SPELL_ADAPTER (self));

  deadline = g_get_monotonic_time () + (G_USEC_PER_SEC/1000L);
  length = _cjh_text_region_get_length (self->region);
  has_more = editor_text_buffer_spell_adapter_update_range (self, 0, length, deadline);

  if (has_more)
    return G_SOURCE_CONTINUE;

  self->update_source = 0;

  return G_SOURCE_REMOVE;
}

static void
editor_text_buffer_spell_adapter_queue_update (EditorTextBufferSpellAdapter *self)
{
  g_assert (EDITOR_IS_TEXT_BUFFER_SPELL_ADAPTER (self));

  if (self->checker == NULL || self->buffer == NULL || !self->enabled)
    {
      g_clear_handle_id (&self->update_source, g_source_remove);
      return;
    }

  if (self->update_source == 0)
    self->update_source = g_timeout_add_full (G_PRIORITY_LOW,
                                              UPDATE_DELAY_MSECS,
                                              (GSourceFunc) editor_text_buffer_spell_adapter_update,
                                              g_object_ref (self),
                                              g_object_unref);
}

static void
editor_text_buffer_spell_adapter_invalidate_all (EditorTextBufferSpellAdapter *self)
{
  gsize length;

  g_assert (EDITOR_IS_TEXT_BUFFER_SPELL_ADAPTER (self));

  length = _cjh_text_region_get_length (self->region);

  if (length > 0)
    {
      _cjh_text_region_replace (self->region, 0, length - 1, UNCHECKED);
      editor_text_buffer_spell_adapter_queue_update (self);
    }
}

static void
on_tag_added_cb (EditorTextBufferSpellAdapter *self,
                 GtkTextTag                   *tag,
                 GtkTextTagTable              *tag_table)
{
  char *name;

  g_assert (EDITOR_IS_TEXT_BUFFER_SPELL_ADAPTER (self));
  g_assert (GTK_IS_TEXT_TAG (tag));
  g_assert (GTK_IS_TEXT_TAG_TABLE (tag_table));

  g_object_get (tag, "name", &name, NULL);
  if (name && strcmp (name, "gtksourceview:context-classes:no-spell-check") == 0)
    {
      g_set_object (&self->no_spell_check_tag, tag);
      editor_text_buffer_spell_adapter_invalidate_all (self);
    }
}

static void
on_tag_removed_cb (EditorTextBufferSpellAdapter *self,
                   GtkTextTag                   *tag,
                   GtkTextTagTable              *tag_table)
{
  g_assert (EDITOR_IS_TEXT_BUFFER_SPELL_ADAPTER (self));
  g_assert (GTK_IS_TEXT_TAG (tag));
  g_assert (GTK_IS_TEXT_TAG_TABLE (tag_table));

  if (tag == self->no_spell_check_tag)
    {
      g_clear_object (&self->no_spell_check_tag);
      editor_text_buffer_spell_adapter_invalidate_all (self);
    }
}

static void
invalidate_tag_region_cb (EditorTextBufferSpellAdapter *self,
                          GtkTextTag                   *tag,
                          GtkTextIter                  *begin,
                          GtkTextIter                  *end,
                          GtkTextBuffer                *buffer)
{
  g_assert (EDITOR_IS_TEXT_BUFFER_SPELL_ADAPTER (self));
  g_assert (GTK_IS_TEXT_TAG (tag));
  g_assert (GTK_IS_TEXT_BUFFER (buffer));

  if (tag == self->no_spell_check_tag)
    {
      gsize begin_offset = gtk_text_iter_get_offset (begin);
      gsize end_offset = gtk_text_iter_get_offset (end);

      _cjh_text_region_replace (self->region, begin_offset, end_offset - begin_offset, UNCHECKED);
      editor_text_buffer_spell_adapter_queue_update (self);
    }
}

static void
editor_text_buffer_spell_adapter_set_buffer (EditorTextBufferSpellAdapter *self,
                                             GtkTextBuffer                *buffer)
{
  g_assert (EDITOR_IS_TEXT_BUFFER_SPELL_ADAPTER (self));
  g_assert (GTK_IS_TEXT_BUFFER (buffer));

  if (g_set_weak_pointer (&self->buffer, buffer))
    {
      GtkTextIter begin, end;
      GtkTextTagTable *tag_table;
      guint offset;
      guint length;

      gtk_text_buffer_get_bounds (buffer, &begin, &end);

      offset = gtk_text_iter_get_offset (&begin);
      length = gtk_text_iter_get_offset (&end) - offset;

      _cjh_text_region_insert (self->region, offset, length, UNCHECKED);

      self->tag = gtk_text_buffer_create_tag (buffer, NULL,
                                              "underline", PANGO_UNDERLINE_ERROR,
                                              NULL);

      /* Track tag changes from the tag table and extract "no-spell-check"
       * tag from GtkSourceView so that we can avoid words with that tag.
       */
      tag_table = gtk_text_buffer_get_tag_table (buffer);
      g_signal_connect_object (tag_table,
                               "tag-added",
                               G_CALLBACK (on_tag_added_cb),
                               self,
                               G_CONNECT_SWAPPED);
      g_signal_connect_object (tag_table,
                               "tag-removed",
                               G_CALLBACK (on_tag_removed_cb),
                               self,
                               G_CONNECT_SWAPPED);

      g_signal_connect_object (buffer,
                               "apply-tag",
                               G_CALLBACK (invalidate_tag_region_cb),
                               self,
                               G_CONNECT_SWAPPED);
      g_signal_connect_object (buffer,
                               "remove-tag",
                               G_CALLBACK (invalidate_tag_region_cb),
                               self,
                               G_CONNECT_SWAPPED);

      editor_text_buffer_spell_adapter_queue_update (self);
    }
}

static void
editor_text_buffer_spell_adapter_finalize (GObject *object)
{
  EditorTextBufferSpellAdapter *self = (EditorTextBufferSpellAdapter *)object;

  g_clear_object (&self->checker);
  g_clear_object (&self->no_spell_check_tag);
  g_clear_pointer (&self->region, _cjh_text_region_free);

  G_OBJECT_CLASS (editor_text_buffer_spell_adapter_parent_class)->finalize (object);
}

static void
editor_text_buffer_spell_adapter_dispose (GObject *object)
{
  EditorTextBufferSpellAdapter *self = (EditorTextBufferSpellAdapter *)object;

  g_clear_weak_pointer (&self->buffer);
  g_clear_handle_id (&self->update_source, g_source_remove);

  G_OBJECT_CLASS (editor_text_buffer_spell_adapter_parent_class)->dispose (object);
}

static void
editor_text_buffer_spell_adapter_get_property (GObject    *object,
                                               guint       prop_id,
                                               GValue     *value,
                                               GParamSpec *pspec)
{
  EditorTextBufferSpellAdapter *self = EDITOR_TEXT_BUFFER_SPELL_ADAPTER (object);

  switch (prop_id)
    {
    case PROP_BUFFER:
      g_value_set_object (value, self->buffer);
      break;

    case PROP_CHECKER:
      g_value_set_object (value, editor_text_buffer_spell_adapter_get_checker (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
editor_text_buffer_spell_adapter_set_property (GObject      *object,
                                               guint         prop_id,
                                               const GValue *value,
                                               GParamSpec   *pspec)
{
  EditorTextBufferSpellAdapter *self = EDITOR_TEXT_BUFFER_SPELL_ADAPTER (object);

  switch (prop_id)
    {
    case PROP_BUFFER:
      editor_text_buffer_spell_adapter_set_buffer (self, g_value_get_object (value));
      break;

    case PROP_CHECKER:
      editor_text_buffer_spell_adapter_set_checker (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
editor_text_buffer_spell_adapter_class_init (EditorTextBufferSpellAdapterClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = editor_text_buffer_spell_adapter_dispose;
  object_class->finalize = editor_text_buffer_spell_adapter_finalize;
  object_class->get_property = editor_text_buffer_spell_adapter_get_property;
  object_class->set_property = editor_text_buffer_spell_adapter_set_property;

  properties [PROP_BUFFER] =
    g_param_spec_object ("buffer",
                         "Buffer",
                         "Buffer",
                         GTK_TYPE_TEXT_BUFFER,
                         (G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  properties [PROP_CHECKER] =
    g_param_spec_object ("checker",
                         "Checker",
                         "Checker",
                         EDITOR_TYPE_SPELL_CHECKER,
                         (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
editor_text_buffer_spell_adapter_init (EditorTextBufferSpellAdapter *self)
{
  self->region = _cjh_text_region_new (NULL, NULL);
}

EditorSpellChecker *
editor_text_buffer_spell_adapter_get_checker (EditorTextBufferSpellAdapter *self)
{
  g_return_val_if_fail (EDITOR_IS_TEXT_BUFFER_SPELL_ADAPTER (self), NULL);

  return self->checker;
}

void
editor_text_buffer_spell_adapter_set_checker (EditorTextBufferSpellAdapter *self,
                                              EditorSpellChecker           *checker)
{
  g_return_if_fail (EDITOR_IS_TEXT_BUFFER_SPELL_ADAPTER (self));
  g_return_if_fail (!checker || EDITOR_IS_SPELL_CHECKER (checker));

  if (g_set_object (&self->checker, checker))
    {
      gsize length = _cjh_text_region_get_length (self->region);

      g_clear_handle_id (&self->update_source, g_source_remove);

      if (length > 0)
        {
          _cjh_text_region_remove (self->region, 0, length - 1);
          _cjh_text_region_insert (self->region, 0, length, UNCHECKED);
          g_assert_cmpint (length, ==, _cjh_text_region_get_length (self->region));
        }

      editor_text_buffer_spell_adapter_queue_update (self);

      g_object_notify_by_pspec (G_OBJECT (self), properties [PROP_CHECKER]);
    }
}

GtkTextBuffer *
editor_text_buffer_spell_adapter_get_buffer (EditorTextBufferSpellAdapter *self)
{
  g_return_val_if_fail (EDITOR_IS_TEXT_BUFFER_SPELL_ADAPTER (self), NULL);

  return self->buffer;
}

void
editor_text_buffer_spell_adapter_insert_text (EditorTextBufferSpellAdapter *self,
                                              guint                         offset,
                                              guint                         length)
{
  g_return_if_fail (EDITOR_IS_TEXT_BUFFER_SPELL_ADAPTER (self));
  g_return_if_fail (length > 0);

  _cjh_text_region_insert (self->region, offset, length, UNCHECKED);
}

void
editor_text_buffer_spell_adapter_delete_range (EditorTextBufferSpellAdapter *self,
                                               guint                         offset,
                                               guint                         length)
{
  g_return_if_fail (EDITOR_IS_TEXT_BUFFER_SPELL_ADAPTER (self));
  g_return_if_fail (self->buffer != NULL);
  g_return_if_fail (length > 0);

  _cjh_text_region_remove (self->region, offset, length);
}

void
editor_text_buffer_spell_adapter_cursor_moved (EditorTextBufferSpellAdapter *self,
                                               guint                         position)
{
  g_return_if_fail (EDITOR_IS_TEXT_BUFFER_SPELL_ADAPTER (self));
  g_return_if_fail (self->buffer != NULL);

  self->cursor_position = position;

  editor_text_buffer_spell_adapter_queue_update (self);
}
