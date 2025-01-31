/*
 * Copyright (C) 2021 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#define G_LOG_DOMAIN "phosh-calls-manager"

#include "config.h"

#include "call.h"
#include "calls-manager.h"
#include "shell.h"
#include "util.h"
#include "dbus/calls-dbus.h"

#define BUS_NAME "org.gnome.Calls"
#define OBJECT_PATH "/org/gnome/Calls"
#define OBJECT_PATHS_CALLS_PREFIX OBJECT_PATH "/Call/"

/**
 * SECTION:calls-manager
 * @short_description: Track ongoing phone calls
 * @Title: PhoshCallsManager
 *
 * #PhoshCallsManager tracks on going calls on the org.gnome.Calls DBus
 * interface and allows interaction with them by wrapping the
 * #PhoshCallsDBusCallsCall proxies in #PhoshCall so all DBus and
 * ObjectManager related logic stays local within #PhoshCallsManager.
 */

enum {
  PROP_0,
  PROP_PRESENT,
  PROP_ACTIVE_CALL,
  PROP_LAST_PROP
};
static GParamSpec *props[PROP_LAST_PROP];


enum {
  CALL_INBOUND,
  CALL_REMOVED,
  N_SIGNALS
};
static guint signals[N_SIGNALS] = { 0 };


struct _PhoshCallsManager {
  PhoshManager                       parent;

  gboolean                           present;
  gboolean                           incoming;
  char                              *active_call;

  PhoshCallsDBusObjectManagerClient *om_client;
  GCancellable                      *cancel;
  GHashTable                        *calls;
};
G_DEFINE_TYPE (PhoshCallsManager, phosh_calls_manager, PHOSH_TYPE_MANAGER);


static gboolean
is_active (PhoshCallState state)
{
  gboolean ret = FALSE;

  if (state == PHOSH_CALL_STATE_ACTIVE ||
      state == PHOSH_CALL_STATE_ALERTING ||
      state == PHOSH_CALL_STATE_DIALING)
    ret = TRUE;

  return ret;
}


static void
on_call_state_changed (PhoshCallsManager       *self,
                       GParamSpec              *pspec,
                       PhoshCallsDBusCallsCall *proxy)
{
  const char *path;
  PhoshCallState state;
  PhoshCall *call;

  g_return_if_fail (PHOSH_IS_CALLS_MANAGER (self));
  g_return_if_fail (PHOSH_CALLS_DBUS_IS_CALLS_CALL (proxy));

  call = g_object_get_data (G_OBJECT (proxy), "call");
  g_return_if_fail (call);

  path = g_dbus_proxy_get_object_path (G_DBUS_PROXY (proxy));
  state = phosh_calls_dbus_calls_call_get_state (proxy);

  g_debug ("Call %s, state %d", path, state);
  if (g_strcmp0 (path, self->active_call) == 0) {
    /* current active call became inactive> */
    if (!is_active (state)) {
      g_debug ("No active call, was %s", path);
      g_clear_pointer (&self->active_call, g_free);
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ACTIVE_CALL]);
      /* TODO: pick new active call from list once calls supports multiple active calls */
    }
    return;
  }

  if (!is_active (state))
      return;

  /* New active call */
  g_free (self->active_call);
  self->active_call = g_strdup (path);
  g_debug ("New active call %s", path);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ACTIVE_CALL]);
}


static void
on_call_proxy_new_for_bus_finish (GObject      *source_object,
                                  GAsyncResult *res,
                                  gpointer     *data)
{
  const char *path;
  gboolean inbound;
  PhoshCallsManager *self;
  g_autoptr (PhoshCallsDBusCallsCall) proxy = NULL;
  g_autoptr (PhoshCall) call = NULL;
  g_autoptr (GError) err = NULL;

  proxy = phosh_calls_dbus_calls_call_proxy_new_for_bus_finish (res, &err);
  if (!proxy) {
    phosh_async_error_warn (err, "Failed to get call proxy");
    return;
  }

  self = PHOSH_CALLS_MANAGER (data);
  path = g_dbus_proxy_get_object_path (G_DBUS_PROXY (proxy));
  if (g_hash_table_contains (self->calls, path)) {
    g_warning ("Already got a call with path %s", path);
    return;
  }

  /* Wrap DBus proxy in PhoshCall */
  call = phosh_call_new (proxy);
  g_object_set_data (G_OBJECT (proxy), "call", call);
  g_hash_table_insert (self->calls, g_strdup (path), g_steal_pointer (&call));

  g_signal_connect_swapped (proxy,
                            "notify::state",
                            G_CALLBACK (on_call_state_changed),
                            self);
  on_call_state_changed (self, NULL, proxy);

  inbound = phosh_calls_dbus_calls_call_get_inbound (proxy);
  g_debug ("Added call %s, inbound: %d", path, inbound);

  if (inbound)
    g_signal_emit (self, signals[CALL_INBOUND], 0, path);
}


static void
on_call_obj_added (PhoshCallsManager *self,
                   GDBusObject       *object)
{
  const char *path;

  g_return_if_fail (PHOSH_IS_CALLS_MANAGER (self));

  path = g_dbus_object_get_object_path (object);
  g_debug ("New call obj at %s", path);
  if (!g_str_has_prefix (path, OBJECT_PATHS_CALLS_PREFIX))
    return;

  phosh_calls_dbus_calls_call_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                 BUS_NAME,
                                                 path,
                                                 self->cancel,
                                                 (GAsyncReadyCallback) on_call_proxy_new_for_bus_finish,
                                                 self);
}


static void
on_call_obj_removed (PhoshCallsManager *self,
                     GDBusObject       *object)
{
  const char *path;

  g_return_if_fail (PHOSH_IS_CALLS_MANAGER (self));

  path = g_dbus_object_get_object_path (object);
  g_debug ("Call obj at %s gone", path);
  if (!g_str_has_prefix (path, OBJECT_PATHS_CALLS_PREFIX))
    return;

  if (g_strcmp0 (path, self->active_call) == 0) {
    g_clear_pointer (&self->active_call, g_free);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ACTIVE_CALL]);
    /* TODO: pick new active call from list once calls supports multiple active calls */
  }

  g_debug ("Removed call %s", path);
  g_signal_emit (self, signals[CALL_REMOVED], 0, path);
  /* This disposes the call object and hence the proxy */
  g_return_if_fail (g_hash_table_remove (self->calls, path));
}


static void
phosh_calls_manager_get_property (GObject    *object,
                                  guint       property_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  PhoshCallsManager *self = PHOSH_CALLS_MANAGER (object);

  switch (property_id) {
  case PROP_PRESENT:
    g_value_set_boolean (value, self->present);
    break;
  case PROP_ACTIVE_CALL:
    g_value_set_string (value, self->active_call);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
on_name_owner_changed (PhoshCallsManager        *self,
                       GParamSpec               *pspec,
                       GDBusObjectManagerClient *om)
{
  g_autofree char *owner = NULL;
  gboolean present;

  g_return_if_fail (PHOSH_IS_CALLS_MANAGER (self));
  g_return_if_fail (G_IS_DBUS_OBJECT_MANAGER_CLIENT (om));

  owner = g_dbus_object_manager_client_get_name_owner (om);
  present = owner ? TRUE : FALSE;

  if (present) {
    g_autolist (GDBusObject) objs = g_dbus_object_manager_get_objects (
      G_DBUS_OBJECT_MANAGER (self->om_client));

    /* Catch up on ongoing calls */
    for (GList *elem = objs; elem; elem = elem->next) {
      on_call_obj_added (self, elem->data);
    }
  } /* else {} is not necessary since we get object-removed signals
     * when name owner quits
     */

  if (present != self->present) {
    self->present = present;
    g_debug ("Calls present: %d", self->present);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PRESENT]);
  }
}


static void
on_om_new_for_bus_finish (GObject      *source_object,
                          GAsyncResult *res,
                          gpointer      data)
{
  g_autoptr (GError) err = NULL;
  PhoshCallsManager *self;
  GDBusObjectManager *om;
  GDBusObjectManagerClient *om_client;

  om = phosh_calls_dbus_object_manager_client_new_for_bus_finish (res, &err);
  if (om == NULL) {
    g_message ("Failed to get calls object manager client: %s", err->message);
    return;
  }

  self = PHOSH_CALLS_MANAGER (data);
  self->om_client = PHOSH_CALLS_DBUS_OBJECT_MANAGER_CLIENT (om);
  om_client = G_DBUS_OBJECT_MANAGER_CLIENT (om);

  g_signal_connect_object (self->om_client,
                           "notify::name-owner",
                           G_CALLBACK (on_name_owner_changed),
                           self,
                           G_CONNECT_SWAPPED);
  on_name_owner_changed (self, NULL, G_DBUS_OBJECT_MANAGER_CLIENT (om));

  g_signal_connect_object (self->om_client,
                           "object-added",
                           G_CALLBACK (on_call_obj_added),
                           self,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (self->om_client,
                           "object-removed",
                           G_CALLBACK (on_call_obj_removed),
                           self,
                           G_CONNECT_SWAPPED);

  g_debug ("Calls manager initialized for name %s at %s",
           g_dbus_object_manager_client_get_name (om_client),
           g_dbus_object_manager_get_object_path (om));
}


static void
phosh_calls_manager_idle_init (PhoshManager *manager)
{
  PhoshCallsManager *self = PHOSH_CALLS_MANAGER (manager);

  phosh_calls_dbus_object_manager_client_new_for_bus (G_BUS_TYPE_SESSION,
                                                      G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START,
                                                      BUS_NAME,
                                                      OBJECT_PATH,
                                                      self->cancel,
                                                      on_om_new_for_bus_finish,
                                                      self);
}


static void
phosh_calls_manager_dispose (GObject *object)
{
  PhoshCallsManager *self = PHOSH_CALLS_MANAGER (object);

  g_cancellable_cancel (self->cancel);
  g_clear_object (&self->cancel);
  g_clear_object (&self->om_client);
  g_clear_pointer (&self->calls, g_hash_table_unref);
  g_clear_pointer (&self->active_call, g_free);

  G_OBJECT_CLASS (phosh_calls_manager_parent_class)->dispose (object);
}


static void
phosh_calls_manager_class_init (PhoshCallsManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  PhoshManagerClass *manager_class = PHOSH_MANAGER_CLASS (klass);

  object_class->get_property = phosh_calls_manager_get_property;
  object_class->dispose = phosh_calls_manager_dispose;

  manager_class->idle_init = phosh_calls_manager_idle_init;

  /**
   * PhoshCallsManager:present:
   *
   * Whether the call interface is present on the bus
   */
  props[PROP_PRESENT] =
    g_param_spec_boolean ("present",
                          "",
                          "",
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * PhoshCallsManager:active-call:
   *
   * The currently active call
   */
  props[PROP_ACTIVE_CALL] =
    g_param_spec_string ("active-call",
                         "",
                         "",
                         NULL,
                         G_PARAM_READABLE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST_PROP, props);

  signals[CALL_INBOUND] = g_signal_new ("call-inbound",
                                        G_TYPE_FROM_CLASS (klass),
                                        G_SIGNAL_RUN_LAST,
                                        0, NULL, NULL,
                                        NULL,
                                        G_TYPE_NONE,
                                        1,
                                        G_TYPE_STRING);

  signals[CALL_REMOVED] = g_signal_new ("call-removed",
                                        G_TYPE_FROM_CLASS (klass),
                                        G_SIGNAL_RUN_LAST,
                                        0, NULL, NULL,
                                        NULL,
                                        G_TYPE_NONE,
                                        1,
                                        G_TYPE_STRING);
}


static void
phosh_calls_manager_init (PhoshCallsManager *self)
{
  self->calls = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  self->cancel = g_cancellable_new ();
}


PhoshCallsManager *
phosh_calls_manager_new (void)
{
  return g_object_new (PHOSH_TYPE_CALLS_MANAGER, NULL);
}


gboolean
phosh_calls_manager_get_present (PhoshCallsManager *self)
{
  g_return_val_if_fail (PHOSH_IS_CALLS_MANAGER (self), FALSE);

  return self->present;
}


int
phosh_calls_manager_get_incoming (PhoshCallsManager *self)
{
  g_return_val_if_fail (PHOSH_IS_CALLS_MANAGER (self), FALSE);

  return self->incoming;
}


const char *
phosh_calls_manager_get_active_call_handle (PhoshCallsManager *self)
{
  g_return_val_if_fail (PHOSH_IS_CALLS_MANAGER (self), NULL);

  return self->active_call;
}


PhoshCall *
phosh_calls_manager_get_call (PhoshCallsManager *self, const char *handle)
{
  g_return_val_if_fail (PHOSH_IS_CALLS_MANAGER (self), NULL);

  return g_hash_table_lookup (self->calls, handle);
}
