/* editor-animation.c
 *
 * Copyright (C) 2010-2016 Christian Hergert <christian@hergert.me>
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version.
 *
 * This file is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define G_LOG_DOMAIN "editor-animation"

#include "config.h"

#include <glib/gi18n.h>
#include <gobject/gvaluecollector.h>
#include <gdk/gdk.h>
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>

#include "editor-animation.h"
#include "editor-frame-source.h"

#define FALLBACK_FRAME_RATE 60

typedef gdouble (*AlphaFunc) (gdouble       offset);
typedef void    (*TweenFunc) (const GValue *begin,
                              const GValue *end,
                              GValue       *value,
                              gdouble       offset);

typedef struct
{
  GParamSpec *pspec;     /* GParamSpec of target property */
  GValue      begin;     /* Begin value in animation */
  GValue      end;       /* End value in animation */
} Tween;


struct _EditorAnimation
{
  GInitiallyUnowned  parent_instance;

  gpointer           target;              /* Target object to animate */
  gint64             begin_time;          /* Time in which animation started */
  gint64             end_time;            /* Deadline for the animation */
  guint              duration_msec;       /* Duration in milliseconds */
  guint              mode;                /* Tween mode */
  gulong             tween_handler;       /* GSource or signal handler */
  gulong             after_paint_handler; /* signal handler */
  gulong             unrealize_handler;   /* GtkWidget::unrealize() callback */
  gdouble            last_offset;         /* Track our last offset */
  GArray            *tweens;              /* Array of tweens to perform */
  GdkFrameClock     *frame_clock;         /* An optional frame-clock for sync. */
  GDestroyNotify     notify;              /* Notify callback */
  gpointer           notify_data;         /* Data for notify */
  guint              stop_called : 1;
};

G_DEFINE_TYPE (EditorAnimation, editor_animation, G_TYPE_INITIALLY_UNOWNED)

enum {
  PROP_0,
  PROP_DURATION,
  PROP_FRAME_CLOCK,
  PROP_MODE,
  PROP_TARGET,
  N_PROPS
};


enum {
  TICK,
  N_SIGNALS
};


/*
 * Helper macros.
 */
#define LAST_FUNDAMENTAL 64
#define TWEEN(type)                                       \
  static void                                             \
  tween_ ## type (const GValue * begin,                   \
                  const GValue * end,                     \
                  GValue * value,                         \
                  gdouble offset)                         \
  {                                                       \
    g ## type x = g_value_get_ ## type (begin);           \
    g ## type y = g_value_get_ ## type (end);             \
    g_value_set_ ## type (value, x + ((y - x) * offset)); \
  }


/*
 * Globals.
 */
static AlphaFunc   alpha_funcs[EDITOR_ANIMATION_LAST];
static gboolean    debug;
static GParamSpec *properties[N_PROPS];
static guint       signals[N_SIGNALS];
static TweenFunc   tween_funcs[LAST_FUNDAMENTAL];
static guint       slow_down_factor = 1;


/*
 * Tweeners for basic types.
 */
TWEEN (int);
TWEEN (uint);
TWEEN (long);
TWEEN (ulong);
TWEEN (float);
TWEEN (double);


/**
 * editor_animation_alpha_ease_in_cubic:
 * @offset: (in): The position within the animation; 0.0 to 1.0.
 *
 * An alpha function to transform the offset within the animation.
 * @EDITOR_ANIMATION_CUBIC means the value will be transformed into
 * cubic acceleration (x * x * x).
 */
static gdouble
editor_animation_alpha_ease_in_cubic (gdouble offset)
{
  return offset * offset * offset;
}


static gdouble
editor_animation_alpha_ease_out_cubic (gdouble offset)
{
  gdouble p = offset - 1.0;

  return p * p * p + 1.0;
}

static gdouble
editor_animation_alpha_ease_in_out_cubic (gdouble offset)
{
  gdouble p = offset * 2.0;

  if (p < 1.0)
    return 0.5 * p * p * p;
  p -= 2.0;
  return 0.5 * (p * p * p + 2.0);
}


/**
 * editor_animation_alpha_linear:
 * @offset: (in): The position within the animation; 0.0 to 1.0.
 *
 * An alpha function to transform the offset within the animation.
 * @EDITOR_ANIMATION_LINEAR means no transformation will be made.
 *
 * Returns: @offset.
 * Side effects: None.
 */
static gdouble
editor_animation_alpha_linear (gdouble offset)
{
  return offset;
}


/**
 * editor_animation_alpha_ease_in_quad:
 * @offset: (in): The position within the animation; 0.0 to 1.0.
 *
 * An alpha function to transform the offset within the animation.
 * @EDITOR_ANIMATION_EASE_IN_QUAD means that the value will be transformed
 * into a quadratic acceleration.
 *
 * Returns: A transformation of @offset.
 * Side effects: None.
 */
static gdouble
editor_animation_alpha_ease_in_quad (gdouble offset)
{
  return offset * offset;
}


/**
 * editor_animation_alpha_ease_out_quad:
 * @offset: (in): The position within the animation; 0.0 to 1.0.
 *
 * An alpha function to transform the offset within the animation.
 * @EDITOR_ANIMATION_EASE_OUT_QUAD means that the value will be transformed
 * into a quadratic deceleration.
 *
 * Returns: A transformation of @offset.
 * Side effects: None.
 */
static gdouble
editor_animation_alpha_ease_out_quad (gdouble offset)
{
  return -1.0 * offset * (offset - 2.0);
}


/**
 * editor_animation_alpha_ease_in_out_quad:
 * @offset: (in): The position within the animation; 0.0 to 1.0.
 *
 * An alpha function to transform the offset within the animation.
 * @EDITOR_ANIMATION_EASE_IN_OUT_QUAD means that the value will be transformed
 * into a quadratic acceleration for the first half, and quadratic
 * deceleration the second half.
 *
 * Returns: A transformation of @offset.
 * Side effects: None.
 */
static gdouble
editor_animation_alpha_ease_in_out_quad (gdouble offset)
{
  offset *= 2.0;
  if (offset < 1.0)
    return 0.5 * offset * offset;
  offset -= 1.0;
  return -0.5 * (offset * (offset - 2.0) - 1.0);
}


/**
 * editor_animation_load_begin_values:
 * @animation: (in): A #EditorAnimation.
 *
 * Load the begin values for all the properties we are about to
 * animate.
 *
 * Side effects: None.
 */
static void
editor_animation_load_begin_values (EditorAnimation *animation)
{
  g_assert (EDITOR_IS_ANIMATION (animation));

  for (guint i = 0; i < animation->tweens->len; i++)
    {
      Tween *tween = &g_array_index (animation->tweens, Tween, i);

      g_value_reset (&tween->begin);
      g_object_get_property (animation->target,
                             tween->pspec->name,
                             &tween->begin);
    }
}


/**
 * editor_animation_unload_begin_values:
 * @animation: (in): A #EditorAnimation.
 *
 * Unloads the begin values for the animation. This might be particularly
 * useful once we support pointer types.
 *
 * Side effects: None.
 */
static void
editor_animation_unload_begin_values (EditorAnimation *animation)
{
  g_assert (EDITOR_IS_ANIMATION (animation));

  for (guint i = 0; i < animation->tweens->len; i++)
    {
      Tween *tween = &g_array_index (animation->tweens, Tween, i);

      g_value_reset (&tween->begin);
    }
}


/**
 * editor_animation_get_offset:
 * @animation: A #EditorAnimation.
 * @frame_time: the time to present the frame, or 0 for current timing.
 *
 * Retrieves the position within the animation from 0.0 to 1.0. This
 * value is calculated using the msec of the beginning of the animation
 * and the current time.
 *
 * Returns: The offset of the animation from 0.0 to 1.0.
 */
static gdouble
editor_animation_get_offset (EditorAnimation *animation,
                             gint64           frame_time)
{
  g_assert (EDITOR_IS_ANIMATION (animation));

  if (frame_time == 0)
    {
      if (animation->frame_clock != NULL)
        frame_time = gdk_frame_clock_get_frame_time (animation->frame_clock);
      else
        frame_time = g_get_monotonic_time ();
    }

  frame_time = CLAMP (frame_time, animation->begin_time, animation->end_time);

  /* Check end_time first in case end_time == begin_time */
  if (frame_time == animation->end_time)
    return 1.0;
  else if (frame_time == animation->begin_time)
    return 0.0;

  return (frame_time - animation->begin_time) / (gdouble)(animation->duration_msec * 1000L);
}


/**
 * editor_animation_update_property:
 * @animation: (in): A #EditorAnimation.
 * @target: (in): A #GObject.
 * @tween: (in): a #Tween containing the property.
 * @value: (in): The new value for the property.
 *
 * Updates the value of a property on an object using @value.
 *
 * Side effects: The property of @target is updated.
 */
static void
editor_animation_update_property (EditorAnimation *animation,
                                  gpointer         target,
                                  Tween           *tween,
                                  const GValue    *value)
{
  g_assert (EDITOR_IS_ANIMATION (animation));
  g_assert (G_IS_OBJECT (target));
  g_assert (tween);
  g_assert (value);

  g_object_set_property (target, tween->pspec->name, value);
}


/**
 * editor_animation_get_value_at_offset:
 * @animation: (in): A #EditorAnimation.
 * @offset: (in): The offset in the animation from 0.0 to 1.0.
 * @tween: (in): A #Tween containing the property.
 * @value: (out): A #GValue in which to store the property.
 *
 * Retrieves a value for a particular position within the animation.
 *
 * Side effects: None.
 */
static void
editor_animation_get_value_at_offset (EditorAnimation *animation,
                                      gdouble          offset,
                                      Tween           *tween,
                                      GValue          *value)
{
  g_assert (EDITOR_IS_ANIMATION (animation));
  g_assert (tween != NULL);
  g_assert (value != NULL);
  g_assert (value->g_type == tween->pspec->value_type);

  if (value->g_type < LAST_FUNDAMENTAL)
    {
      /*
       * If you hit the following assertion, you need to add a function
       * to create the new value at the given offset.
       */
      g_assert (tween_funcs[value->g_type]);
      tween_funcs[value->g_type](&tween->begin, &tween->end, value, offset);
    }
  else
    {
      /*
       * TODO: Support complex transitions.
       */
      if (offset >= 1.0)
        g_value_copy (&tween->end, value);
    }
}


static void
editor_animation_set_frame_clock (EditorAnimation *animation,
                                  GdkFrameClock   *frame_clock)
{
  g_set_object (&animation->frame_clock, frame_clock);
}


static void
editor_animation_target_unrealize_cb (EditorAnimation *animation,
                                      GtkWidget       *widget)
{
  g_assert (EDITOR_IS_ANIMATION (animation));
  g_assert (GTK_IS_WIDGET (widget));

  /* When we are called as part of an unrealize callback, we lose
   * access to our frame clock and the signal handlers can no
   * longer be removed (as they've been disposed). So just clear
   * them so we don't try to disconnect them incorrectly.
   */
  g_clear_signal_handler (&animation->unrealize_handler, widget);
  animation->tween_handler = 0;
  animation->after_paint_handler = 0;

  editor_animation_stop (animation);
}


static void
editor_animation_set_target (EditorAnimation *animation,
                             gpointer         target)
{
  g_assert (!animation->target);

  animation->target = g_object_ref (target);

  if (GTK_IS_WIDGET (animation->target))
    {
      animation->unrealize_handler =
        g_signal_connect_swapped (animation->target,
                                  "unrealize",
                                  G_CALLBACK (editor_animation_target_unrealize_cb),
                                  animation);
      editor_animation_set_frame_clock (animation,
                                        gtk_widget_get_frame_clock (animation->target));
    }
}

/**
 * editor_animation_tick:
 * @animation: (in): A #EditorAnimation.
 *
 * Moves the object properties to the next position in the animation.
 *
 * Returns: %TRUE if the animation has not completed; otherwise %FALSE.
 * Side effects: None.
 */
static gboolean
editor_animation_tick (EditorAnimation *animation,
                       gdouble          offset)
{
  gdouble alpha;
  GValue value = { 0 };
  Tween *tween;
  guint i;

  g_assert (EDITOR_IS_ANIMATION (animation));

  if (offset == animation->last_offset)
    return offset < 1.0;

  alpha = alpha_funcs[animation->mode](offset);

  /*
   * Update property values.
   */
  for (i = 0; i < animation->tweens->len; i++)
    {
      tween = &g_array_index (animation->tweens, Tween, i);
      g_value_init (&value, tween->pspec->value_type);
      editor_animation_get_value_at_offset (animation, alpha, tween, &value);
      editor_animation_update_property (animation,
                                        animation->target,
                                        tween,
                                        &value);
      g_value_unset (&value);
    }

  /*
   * Notify anyone interested in the tick signal.
   */
  g_signal_emit (animation, signals[TICK], 0);

  /*
   * Flush any outstanding events to the graphics server (in the case of X).
   */
#if !GTK_CHECK_VERSION (3, 13, 0)
  if (GTK_IS_WIDGET (animation->target))
    {
      GdkWindow *window;

      if ((window = gtk_widget_get_window (GTK_WIDGET (animation->target))))
        gdk_window_flush (window);
    }
#endif

  animation->last_offset = offset;

  return offset < 1.0;
}


/**
 * editor_animation_timeout_cb:
 * @user_data: (in): A #EditorAnimation.
 *
 * Timeout from the main loop to move to the next step of the animation.
 *
 * Returns: %TRUE until the animation has completed; otherwise %FALSE.
 * Side effects: None.
 */
static gboolean
editor_animation_timeout_cb (gpointer user_data)
{
  EditorAnimation *animation = user_data;
  gboolean ret;
  gdouble offset;

  offset = editor_animation_get_offset (animation, 0);

  if (!(ret = editor_animation_tick (animation, offset)))
    editor_animation_stop (animation);

  return ret;
}


static gboolean
editor_animation_widget_tick_cb (GdkFrameClock   *frame_clock,
                                 EditorAnimation *animation)
{
  gboolean ret = G_SOURCE_REMOVE;

  g_assert (GDK_IS_FRAME_CLOCK (frame_clock));
  g_assert (EDITOR_IS_ANIMATION (animation));

  if (animation->tween_handler)
    {
      gdouble offset;

      offset = editor_animation_get_offset (animation, 0);

      if (!(ret = editor_animation_tick (animation, offset)))
        editor_animation_stop (animation);
    }

  return ret;
}


static void
editor_animation_widget_after_paint_cb (GdkFrameClock   *frame_clock,
                                        EditorAnimation *animation)
{
  gint64 base_time;
  gint64 interval;
  gint64 next_frame_time;
  gdouble offset;

  g_assert (GDK_IS_FRAME_CLOCK (frame_clock));
  g_assert (EDITOR_IS_ANIMATION (animation));

  base_time = gdk_frame_clock_get_frame_time (frame_clock);
  gdk_frame_clock_get_refresh_info (frame_clock, base_time, &interval, &next_frame_time);

  offset = editor_animation_get_offset (animation, next_frame_time);

  editor_animation_tick (animation, offset);
}


/**
 * editor_animation_start:
 * @animation: (in): A #EditorAnimation.
 *
 * Start the animation. When the animation stops, the internal reference will
 * be dropped and the animation may be finalized.
 *
 * Side effects: None.
 */
void
editor_animation_start (EditorAnimation *animation)
{
  g_return_if_fail (EDITOR_IS_ANIMATION (animation));
  g_return_if_fail (!animation->tween_handler);

  g_object_ref_sink (animation);
  editor_animation_load_begin_values (animation);

  /*
   * We want the real current time instead of the GdkFrameClocks current time
   * because if the clock was asleep, it could be innaccurate.
   */

  if (animation->frame_clock)
    {
      animation->begin_time = gdk_frame_clock_get_frame_time (animation->frame_clock);
      animation->end_time = animation->begin_time + (animation->duration_msec * 1000L);
      animation->tween_handler =
        g_signal_connect_object (animation->frame_clock,
                                 "update",
                                 G_CALLBACK (editor_animation_widget_tick_cb),
                                 animation,
                                 0);
      animation->after_paint_handler =
        g_signal_connect_object (animation->frame_clock,
                                 "after-paint",
                                 G_CALLBACK (editor_animation_widget_after_paint_cb),
                                 animation,
                                 0);
      gdk_frame_clock_begin_updating (animation->frame_clock);
    }
  else
    {
      animation->begin_time = g_get_monotonic_time ();
      animation->end_time = animation->begin_time + (animation->duration_msec * 1000L);
      animation->tween_handler = editor_frame_source_add (FALLBACK_FRAME_RATE,
                                                          editor_animation_timeout_cb,
                                                          animation);
    }
}


static void
editor_animation_notify (EditorAnimation *self)
{
  g_assert (EDITOR_IS_ANIMATION (self));

  if (self->notify != NULL)
    {
      GDestroyNotify notify = self->notify;
      gpointer data = self->notify_data;

      self->notify = NULL;
      self->notify_data = NULL;

      notify (data);
    }
}


/**
 * editor_animation_stop:
 * @animation: (nullable): A #EditorAnimation.
 *
 * Stops a running animation. The internal reference to the animation is
 * dropped and therefore may cause the object to finalize.
 *
 * As a convenience, this function accepts %NULL for @animation but
 * does nothing if that should occur.
 */
void
editor_animation_stop (EditorAnimation *animation)
{
  if (animation == NULL)
    return;

  g_return_if_fail (EDITOR_IS_ANIMATION (animation));

  if (animation->stop_called)
    return;

  animation->stop_called = TRUE;

  if (animation->tween_handler)
    {
      if (animation->frame_clock)
        {
          g_assert (GDK_IS_FRAME_CLOCK (animation->frame_clock));

          gdk_frame_clock_end_updating (animation->frame_clock);
          g_clear_signal_handler (&animation->tween_handler, animation->frame_clock);
          g_clear_signal_handler (&animation->after_paint_handler, animation->frame_clock);
          animation->tween_handler = 0;
        }
      else
        {
          g_source_remove (animation->tween_handler);
          animation->tween_handler = 0;
        }

      if (GTK_IS_WIDGET (animation->target))
        g_clear_signal_handler (&animation->unrealize_handler, animation->target);

      editor_animation_unload_begin_values (animation);
      editor_animation_notify (animation);
      g_object_unref (animation);
    }
}


/**
 * editor_animation_add_property:
 * @animation: (in): A #EditorAnimation.
 * @pspec: (in): A #ParamSpec of @target or a #GtkWidget<!-- -->'s parent.
 * @value: (in): The new value for the property at the end of the animation.
 *
 * Adds a new property to the set of properties to be animated during the
 * lifetime of the animation.
 *
 * Side effects: None.
 */
void
editor_animation_add_property (EditorAnimation *animation,
                               GParamSpec      *pspec,
                               const GValue    *value)
{
  Tween tween = { 0 };

  g_return_if_fail (EDITOR_IS_ANIMATION (animation));
  g_return_if_fail (pspec != NULL);
  g_return_if_fail (value != NULL);
  g_return_if_fail (value->g_type);
  g_return_if_fail (animation->target);
  g_return_if_fail (!animation->tween_handler);

  tween.pspec = g_param_spec_ref (pspec);
  g_value_init (&tween.begin, pspec->value_type);
  g_value_init (&tween.end, pspec->value_type);
  g_value_copy (value, &tween.end);
  g_array_append_val (animation->tweens, tween);
}


/**
 * editor_animation_dispose:
 * @object: (in): A #EditorAnimation.
 *
 * Releases any object references the animation contains.
 *
 * Side effects: None.
 */
static void
editor_animation_dispose (GObject *object)
{
  EditorAnimation *self = EDITOR_ANIMATION (object);

  g_clear_object (&self->target);
  g_clear_object (&self->frame_clock);

  G_OBJECT_CLASS (editor_animation_parent_class)->dispose (object);
}


/**
 * editor_animation_finalize:
 * @object: (in): A #EditorAnimation.
 *
 * Finalizes the object and releases any resources allocated.
 *
 * Side effects: None.
 */
static void
editor_animation_finalize (GObject *object)
{
  EditorAnimation *self = EDITOR_ANIMATION (object);
  Tween *tween;
  guint i;

  for (i = 0; i < self->tweens->len; i++)
    {
      tween = &g_array_index (self->tweens, Tween, i);
      g_value_unset (&tween->begin);
      g_value_unset (&tween->end);
      g_param_spec_unref (tween->pspec);
    }

  g_array_unref (self->tweens);

  G_OBJECT_CLASS (editor_animation_parent_class)->finalize (object);
}


/**
 * editor_animation_set_property:
 * @object: (in): A #GObject.
 * @prop_id: (in): The property identifier.
 * @value: (in): The given property.
 * @pspec: (in): A #ParamSpec.
 *
 * Set a given #GObject property.
 */
static void
editor_animation_set_property (GObject      *object,
                               guint         prop_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
  EditorAnimation *animation = EDITOR_ANIMATION (object);

  switch (prop_id)
    {
    case PROP_DURATION:
      animation->duration_msec = g_value_get_uint (value) * slow_down_factor;
      break;

    case PROP_FRAME_CLOCK:
      editor_animation_set_frame_clock (animation, g_value_get_object (value));
      break;

    case PROP_MODE:
      animation->mode = g_value_get_enum (value);
      break;

    case PROP_TARGET:
      editor_animation_set_target (animation, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


/**
 * editor_animation_class_init:
 * @klass: (in): A #EditorAnimationClass.
 *
 * Initializes the GObjectClass.
 *
 * Side effects: Properties, signals, and vtables are initialized.
 */
static void
editor_animation_class_init (EditorAnimationClass *klass)
{
  GObjectClass *object_class;
  const gchar *slow_down_factor_env;

  debug = !!g_getenv ("EDITOR_ANIMATION_DEBUG");
  slow_down_factor_env = g_getenv ("EDITOR_ANIMATION_SLOW_DOWN_FACTOR");

  if (slow_down_factor_env)
    slow_down_factor = MAX (1, atoi (slow_down_factor_env));

  object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = editor_animation_dispose;
  object_class->finalize = editor_animation_finalize;
  object_class->set_property = editor_animation_set_property;

  /**
   * EditorAnimation:duration:
   *
   * The "duration" property is the total number of milliseconds that the
   * animation should run before being completed.
   */
  properties[PROP_DURATION] =
    g_param_spec_uint ("duration",
                       "Duration",
                       "The duration of the animation",
                       0,
                       G_MAXUINT,
                       250,
                       (G_PARAM_WRITABLE |
                        G_PARAM_CONSTRUCT_ONLY |
                        G_PARAM_STATIC_STRINGS));

  properties[PROP_FRAME_CLOCK] =
    g_param_spec_object ("frame-clock",
                         "Frame Clock",
                         "An optional frame-clock to synchronize with.",
                         GDK_TYPE_FRAME_CLOCK,
                         (G_PARAM_WRITABLE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS));

  /**
   * EditorAnimation:mode:
   *
   * The "mode" property is the Alpha function that should be used to
   * determine the offset within the animation based on the current
   * offset in the animations duration.
   */
  properties[PROP_MODE] =
    g_param_spec_enum ("mode",
                       "Mode",
                       "The animation mode",
                       EDITOR_TYPE_ANIMATION_MODE,
                       EDITOR_ANIMATION_LINEAR,
                       (G_PARAM_WRITABLE |
                        G_PARAM_CONSTRUCT_ONLY |
                        G_PARAM_STATIC_STRINGS));

  /**
   * EditorAnimation:target:
   *
   * The "target" property is the #GObject that should have its properties
   * animated.
   */
  properties[PROP_TARGET] =
    g_param_spec_object ("target",
                         "Target",
                         "The target of the animation",
                         G_TYPE_OBJECT,
                         (G_PARAM_WRITABLE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, N_PROPS, properties);

  /**
   * EditorAnimation::tick:
   *
   * The "tick" signal is emitted on each frame in the animation.
   */
  signals[TICK] = g_signal_new ("tick",
                                 EDITOR_TYPE_ANIMATION,
                                 G_SIGNAL_RUN_FIRST,
                                 0,
                                 NULL, NULL, NULL,
                                 G_TYPE_NONE,
                                 0);

#define SET_ALPHA(_T, _t) \
  alpha_funcs[EDITOR_ANIMATION_ ## _T] = editor_animation_alpha_ ## _t

  SET_ALPHA (LINEAR, linear);
  SET_ALPHA (EASE_IN_QUAD, ease_in_quad);
  SET_ALPHA (EASE_OUT_QUAD, ease_out_quad);
  SET_ALPHA (EASE_IN_OUT_QUAD, ease_in_out_quad);
  SET_ALPHA (EASE_IN_CUBIC, ease_in_cubic);
  SET_ALPHA (EASE_OUT_CUBIC, ease_out_cubic);
  SET_ALPHA (EASE_IN_OUT_CUBIC, ease_in_out_cubic);

#define SET_TWEEN(_T, _t) \
  G_STMT_START { \
    guint idx = G_TYPE_ ## _T; \
    tween_funcs[idx] = tween_ ## _t; \
  } G_STMT_END

  SET_TWEEN (INT, int);
  SET_TWEEN (UINT, uint);
  SET_TWEEN (LONG, long);
  SET_TWEEN (ULONG, ulong);
  SET_TWEEN (FLOAT, float);
  SET_TWEEN (DOUBLE, double);
}


/**
 * editor_animation_init:
 * @animation: (in): A #EditorAnimation.
 *
 * Initializes the #EditorAnimation instance.
 *
 * Side effects: Everything.
 */
static void
editor_animation_init (EditorAnimation *animation)
{
  animation->duration_msec = 250;
  animation->mode = EDITOR_ANIMATION_EASE_IN_OUT_QUAD;
  animation->tweens = g_array_new (FALSE, FALSE, sizeof (Tween));
  animation->last_offset = -G_MINDOUBLE;
}


/**
 * editor_animation_mode_get_type:
 *
 * Retrieves the GType for #EditorAnimationMode.
 *
 * Returns: A GType.
 * Side effects: GType registered on first call.
 */
GType
editor_animation_mode_get_type (void)
{
  static GType type_id = 0;
  static const GEnumValue values[] = {
    { EDITOR_ANIMATION_LINEAR, "EDITOR_ANIMATION_LINEAR", "linear" },
    { EDITOR_ANIMATION_EASE_IN_QUAD, "EDITOR_ANIMATION_EASE_IN_QUAD", "ease-in-quad" },
    { EDITOR_ANIMATION_EASE_IN_OUT_QUAD, "EDITOR_ANIMATION_EASE_IN_OUT_QUAD", "ease-in-out-quad" },
    { EDITOR_ANIMATION_EASE_OUT_QUAD, "EDITOR_ANIMATION_EASE_OUT_QUAD", "ease-out-quad" },
    { EDITOR_ANIMATION_EASE_IN_CUBIC, "EDITOR_ANIMATION_EASE_IN_CUBIC", "ease-in-cubic" },
    { EDITOR_ANIMATION_EASE_OUT_CUBIC, "EDITOR_ANIMATION_EASE_OUT_CUBIC", "ease-out-cubic" },
    { EDITOR_ANIMATION_EASE_IN_OUT_CUBIC, "EDITOR_ANIMATION_EASE_IN_OUT_CUBIC", "ease-in-out-cubic" },
    { 0 }
  };

  if (G_UNLIKELY (!type_id))
    type_id = g_enum_register_static ("EditorAnimationMode", values);
  return type_id;
}

/**
 * editor_object_animatev:
 * @object: A #GObject.
 * @mode: The animation mode.
 * @duration_msec: The duration in milliseconds.
 * @frame_clock: (nullable): The #GdkFrameClock to synchronize to.
 * @first_property: The first property to animate.
 * @args: A variadac list of arguments
 *
 * Returns: (transfer none): A #EditorAnimation.
 */
EditorAnimation *
editor_object_animatev (gpointer             object,
                        EditorAnimationMode  mode,
                        guint                duration_msec,
                        GdkFrameClock       *frame_clock,
                        const gchar         *first_property,
                        va_list              args)
{
  EditorAnimation *animation;
  GObjectClass *klass;
  const gchar *name;
  GParamSpec *pspec;
  GValue value = { 0 };
  gchar *error = NULL;
  gboolean enable_animations;

  g_return_val_if_fail (first_property != NULL, NULL);
  g_return_val_if_fail (mode < EDITOR_ANIMATION_LAST, NULL);

  if ((frame_clock == NULL) && GTK_IS_WIDGET (object))
    frame_clock = gtk_widget_get_frame_clock (GTK_WIDGET (object));

  /*
   * If we have a frame clock, then we must be in the gtk thread and we
   * should check GtkSettings for disabled animations. If we are disabled,
   * we will just make the timeout immediate.
   */
  if (frame_clock != NULL)
    {
      g_object_get (gtk_settings_get_default (),
                    "gtk-enable-animations", &enable_animations,
                    NULL);

      if (enable_animations == FALSE)
        duration_msec = 0;
    }

  name = first_property;
  klass = G_OBJECT_GET_CLASS (object);
  animation = g_object_new (EDITOR_TYPE_ANIMATION,
                            "duration", duration_msec,
                            "frame-clock", frame_clock,
                            "mode", mode,
                            "target", object,
                            NULL);

  do
    {
      /*
       * First check for the property on the object. If that does not exist
       * then check if the object has a parent and look at its child
       * properties (if it's a GtkWidget).
       */
      if (!(pspec = g_object_class_find_property (klass, name)))
        {
          g_critical (_("Failed to find property %s in %s"),
                      name, G_OBJECT_CLASS_NAME (klass));
          goto failure;
        }

      g_value_init (&value, pspec->value_type);
      G_VALUE_COLLECT (&value, args, 0, &error);
      if (error != NULL)
        {
          g_critical (_("Failed to retrieve va_list value: %s"), error);
          g_free (error);
          goto failure;
        }

      editor_animation_add_property (animation, pspec, &value);
      g_value_unset (&value);
    }
  while ((name = va_arg (args, const gchar *)));

  editor_animation_start (animation);

  return animation;

failure:
  g_object_ref_sink (animation);
  g_object_unref (animation);
  return NULL;
}

/**
 * editor_object_animate:
 * @object: (in): A #GObject.
 * @mode: (in): The animation mode.
 * @duration_msec: (in): The duration in milliseconds.
 * @first_property: (in): The first property to animate.
 *
 * Animates the properties of @object. The can be set in a similar manner to g_object_set(). They
 * will be animated from their current value to the target value over the time period.
 *
 * Return value: (transfer none): A #EditorAnimation.
 * Side effects: None.
 */
EditorAnimation*
editor_object_animate (gpointer             object,
                       EditorAnimationMode  mode,
                       guint                duration_msec,
                       GdkFrameClock       *frame_clock,
                       const gchar         *first_property,
                       ...)
{
  EditorAnimation *animation;
  va_list args;

  va_start (args, first_property);
  animation = editor_object_animatev (object, mode, duration_msec, frame_clock, first_property, args);
  va_end (args);

  return animation;
}

/**
 * editor_object_animate_full:
 *
 * Return value: (transfer none): A #EditorAnimation.
 */
EditorAnimation*
editor_object_animate_full (gpointer             object,
                            EditorAnimationMode  mode,
                            guint                duration_msec,
                            GdkFrameClock       *frame_clock,
                            GDestroyNotify       notify,
                            gpointer             notify_data,
                            const gchar         *first_property,
                            ...)
{
  EditorAnimation *animation;
  va_list args;

  va_start (args, first_property);
  animation = editor_object_animatev (object, mode, duration_msec, frame_clock, first_property, args);
  va_end (args);

  animation->notify = notify;
  animation->notify_data = notify_data;

  return animation;
}

guint
editor_animation_calculate_duration (GdkMonitor *monitor,
                                     gdouble     from_value,
                                     gdouble     to_value)
{
  GdkRectangle geom;
  gdouble distance_units;
  gdouble distance_mm;
  gdouble mm_per_frame;
  gint height_mm;
  gint refresh_rate;
  gint n_frames;
  guint ret;

#define MM_PER_SECOND       (150.0)
#define MIN_FRAMES_PER_ANIM (5)
#define MAX_FRAMES_PER_ANIM (500)

  g_assert (GDK_IS_MONITOR (monitor));
  g_assert (from_value >= 0.0);
  g_assert (to_value >= 0.0);

  /*
   * Get various monitor information we'll need to calculate the duration of
   * the animation. We need the physical space of the monitor, the refresh
   * rate, and geometry so that we can limit how many device units we will
   * traverse per-frame of the animation. Failure to deal with the physical
   * space results in jittery animations to the user.
   *
   * It would also be nice to take into account the acceleration curve so that
   * we know the max amount of jump per frame, but that is getting into
   * diminishing returns since we can just average it out.
   */
  height_mm = gdk_monitor_get_height_mm (monitor);
  gdk_monitor_get_geometry (monitor, &geom);
  refresh_rate = gdk_monitor_get_refresh_rate (monitor);
  if (refresh_rate == 0)
    refresh_rate = 60000;

  /*
   * The goal here is to determine the number of millimeters that we need to
   * animate given a transition of distance_unit pixels. Since we are dealing
   * with physical units (mm), we don't need to take into account the device
   * scale underneath the widget. The equation comes out the same.
   */

  distance_units = ABS (from_value - to_value);
  distance_mm = distance_units / (gdouble)geom.height * height_mm;
  mm_per_frame = MM_PER_SECOND / (refresh_rate / 1000.0);
  n_frames = (distance_mm / mm_per_frame) + 1;

  ret = n_frames * (1000.0 / (refresh_rate / 1000.0));
  ret = CLAMP (ret,
               MIN_FRAMES_PER_ANIM * (1000000.0 / refresh_rate),
               MAX_FRAMES_PER_ANIM * (1000000.0 / refresh_rate));

  return ret;

#undef MM_PER_SECOND
#undef MIN_FRAMES_PER_ANIM
#undef MAX_FRAMES_PER_ANIM
}
