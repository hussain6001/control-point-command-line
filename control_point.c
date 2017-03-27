#include <libgupnp/gupnp-control-point.h>
#include <libgupnp-av/gupnp-av.h>
#include <string.h>
#include <semaphore.h>

typedef void (* MetadataFunc) (const char *metadata,
                               gpointer    user_data);

#define MEDIA_RENDERER "urn:schemas-upnp-org:device:MediaRenderer:1"
#define MEDIA_SERVER "urn:schemas-upnp-org:device:MediaServer:1"
#define CONTENT_DIR "urn:schemas-upnp-org:service:ContentDirectory"

#define CONNECTION_MANAGER "urn:schemas-upnp-org:service:ConnectionManager"
#define AV_TRANSPORT "urn:schemas-upnp-org:service:AVTransport"
#define RENDERING_CONTROL "urn:schemas-upnp-org:service:RenderingControl"


#define OBJECT_CLASS_CONTAINER "object.container"

#define MAX_BROWSE 64

static int upnp_port = 0;

static GUPnPContextManager *context_manager;

static GHashTable *server_table = NULL;
static GHashTable *browse_table = NULL;
static GHashTable *renderer_table = NULL;

static sem_t browse_sem, play_sem, duration_sem;

static char current_renderer[256];


typedef enum 
{
	PLAYING,
	PAUSED,
	STOPPED
} player_status;

static player_status current_status = STOPPED;


typedef struct
{
	char *title;
	char *parent_id;
	char *class;
        GUPnPDIDLLiteResource *resource;

} Container;

typedef struct
{
	char *friendly_name;
        GUPnPServiceProxy *content_dir;
	GUPnPDeviceInfo  *info;
} MediaServers;

typedef struct {
	char *friendly_name;
	GUPnPServiceProxy *av_transport;
	GUPnPServiceProxy *cm;
	GUPnPServiceProxy *rendering_control;

	char *sink_protocol_info;
} RendererData;
	
typedef struct
{
	GUPnPServiceProxy *content_dir;

	gchar *id;

	guint32 starting_index;
} BrowseData;

typedef struct
{
	MetadataFunc callback;

	gchar *id;

	gpointer user_data;
} BrowseMetadataData;

typedef struct
{
	GCallback callback;

	GUPnPDIDLLiteResource *resource;
} SetAVTransportURIData;


static GUPnPServiceProxy *
get_content_dir (GUPnPDeviceProxy *proxy)
{
        GUPnPDeviceInfo  *info;
        GUPnPServiceInfo *content_dir;

        info = GUPNP_DEVICE_INFO (proxy);

        content_dir = gupnp_device_info_get_service (info, CONTENT_DIR);

        return GUPNP_SERVICE_PROXY (content_dir);
}


void add_media_server(GUPnPDeviceProxy  *proxy)
{
	GUPnPDeviceInfo   *info;
        GUPnPServiceProxy *content_dir;
        char              *friendly_name;
	int i;
	gboolean server_present = FALSE;
	char *udn;


	info = GUPNP_DEVICE_INFO (proxy);
        content_dir = get_content_dir (proxy);
        friendly_name = gupnp_device_info_get_friendly_name (info);
	
	udn = g_strdup(gupnp_device_info_get_udn(info));

	
	if(NULL == g_hash_table_lookup(server_table,udn))
	{
		MediaServers *server = (MediaServers*)malloc(sizeof(MediaServers)); 
		
		server->friendly_name = friendly_name;
		server->content_dir = content_dir;
		server->info = info;
		
		g_hash_table_insert(server_table, udn, server);

		server_present = TRUE;
			
	}

}

static void
get_protocol_info_cb (GUPnPServiceProxy       *cm,
                      GUPnPServiceProxyAction *action,
                      gpointer                 user_data)
{

        gchar      *sink_protocol_info;
        char *udn;
        GError      *error;

        udn = g_strdup(gupnp_service_info_get_udn (GUPNP_SERVICE_INFO (cm)));

        error = NULL;
        if (!gupnp_service_proxy_end_action (cm,
                                             action,
                                             &error,
                                             "Sink",
                                             G_TYPE_STRING,
                                             &sink_protocol_info,
                                             NULL)) {
                g_warning ("Failed to get sink protocol info from "
                           "media renderer '%s':%s\n",
                           udn,
                           error->message);
                g_error_free (error);

                goto return_point;
        }

        if (sink_protocol_info) {
		RendererData *data;
		data = (RendererData*)g_hash_table_lookup(renderer_table, udn);
		data->sink_protocol_info = sink_protocol_info;
        }

return_point:
        g_object_unref (cm);
}

static GUPnPServiceProxy *
get_connection_manager (GUPnPDeviceProxy *proxy)
{

        GUPnPDeviceInfo  *info;
        GUPnPServiceInfo *cm;

        info = GUPNP_DEVICE_INFO (proxy);

        cm = gupnp_device_info_get_service (info, CONNECTION_MANAGER);

        return GUPNP_SERVICE_PROXY (cm);
}

static GUPnPServiceProxy *
get_rendering_control (GUPnPDeviceProxy *proxy)
{

        GUPnPDeviceInfo  *info;
        GUPnPServiceInfo *rendering_control;

        info = GUPNP_DEVICE_INFO (proxy);

        rendering_control = gupnp_device_info_get_service (info,
                                                           RENDERING_CONTROL);

        return GUPNP_SERVICE_PROXY (rendering_control);
}

static GUPnPServiceProxy *
get_av_transport (GUPnPDeviceProxy *renderer)
{

        GUPnPDeviceInfo  *info;
        GUPnPServiceInfo *av_transport;

        info = GUPNP_DEVICE_INFO (renderer);

        av_transport = gupnp_device_info_get_service (info, AV_TRANSPORT);

        return GUPNP_SERVICE_PROXY (av_transport);
}


void
add_media_renderer (GUPnPDeviceProxy *proxy)
{

        char        *udn;
        GUPnPServiceProxy *cm;
        GUPnPServiceProxy *av_transport;
        GUPnPServiceProxy *rendering_control;
	GUPnPDeviceInfo  *info;
	char *name;
	

        udn = g_strdup(gupnp_device_info_get_udn (GUPNP_DEVICE_INFO (proxy)));
        if (udn == NULL)
                return;

        cm = get_connection_manager (proxy);
        if (G_UNLIKELY (cm == NULL))
                return;


        av_transport = get_av_transport (proxy);
        if (av_transport == NULL)
		goto no_av_transport;


        rendering_control = get_rendering_control (proxy);
        if (rendering_control == NULL) 
                goto no_rendering_control;


        gupnp_service_proxy_begin_action (g_object_ref (cm),
					  "GetProtocolInfo",
                                          get_protocol_info_cb,
                                          NULL,
                                          NULL);
	info = GUPNP_DEVICE_INFO (proxy);
	name = gupnp_device_info_get_friendly_name (info);
	if (name == NULL)
                name = g_strdup (udn);

	if(NULL == g_hash_table_lookup(renderer_table, udn)){
		RendererData *renderer = (RendererData*)malloc(sizeof(RendererData));

		renderer->friendly_name = name;
		renderer->av_transport = av_transport;
		renderer->cm= cm;
		renderer->rendering_control = rendering_control;
		renderer->sink_protocol_info = NULL;
		
	
		g_hash_table_insert(renderer_table, udn, renderer);

	}
	
	
/*
        gupnp_service_proxy_begin_action (g_object_ref (av_transport),
                                          "GetTransportInfo",
                                          get_transport_info_cb,
                                          NULL,
                                          "InstanceID", G_TYPE_UINT, 0,
                                          NULL);

        gupnp_service_proxy_begin_action (g_object_ref (av_transport),
                                          "GetMediaInfo",
                                          get_media_info_cb,
                                          NULL,
                                          "InstanceID", G_TYPE_UINT, 0,
                                          NULL);

        gupnp_service_proxy_begin_action (g_object_ref (rendering_control),
                                          "GetVolume",
                                          get_volume_cb,
                                          NULL,
                                          "InstanceID", G_TYPE_UINT, 0,
                                          "Channel", G_TYPE_STRING, "Master",
                                          NULL);
*/
//	g_object_unref (rendering_control);
no_rendering_control:
	puts("No Rendering Control");
//        g_object_unref (av_transport);
no_av_transport:
	puts("No AV Transport");
//        g_object_unref (cm);
	
}


void remove_media_server(GUPnPDeviceProxy  *proxy)
{
	GUPnPDeviceInfo   *info;
	char *udn;

	info = GUPNP_DEVICE_INFO(proxy);
	udn = g_strdup(gupnp_device_info_get_udn(info));

	g_hash_table_remove(server_table, udn);
}

void
remove_media_renderer (GUPnPDeviceProxy *proxy)
{
	GUPnPDeviceInfo   *info;
	char *udn;

	info = GUPNP_DEVICE_INFO(proxy);
	udn = g_strdup(gupnp_device_info_get_udn(info));

	g_hash_table_remove(renderer_table, udn);
}

static void
dms_proxy_available_cb (GUPnPControlPoint *cp,
                        GUPnPDeviceProxy  *proxy)
{
        add_media_server (proxy);
}

static void
dms_proxy_unavailable_cb (GUPnPControlPoint *cp,
                          GUPnPDeviceProxy  *proxy)
{
        remove_media_server (proxy);
}

static void
dmr_proxy_available_cb (GUPnPControlPoint *cp,
                        GUPnPDeviceProxy  *proxy)
{

        add_media_renderer (proxy);
}

static void
dmr_proxy_unavailable_cb (GUPnPControlPoint *cp,
                          GUPnPDeviceProxy  *proxy)
{

        remove_media_renderer (proxy);
}


static void
on_context_available (GUPnPContextManager *context_manager,
                      GUPnPContext        *context,
                      gpointer             user_data)
{
        GUPnPControlPoint *dms_cp;
        GUPnPControlPoint *dmr_cp;

        dms_cp = gupnp_control_point_new (context, MEDIA_SERVER);
	dmr_cp = gupnp_control_point_new (context, MEDIA_RENDERER);

        g_signal_connect (dms_cp,
                          "device-proxy-available",
                          G_CALLBACK (dms_proxy_available_cb),
                          NULL);
        g_signal_connect (dms_cp,
                          "device-proxy-unavailable",
                          G_CALLBACK (dms_proxy_unavailable_cb),
                          NULL);

	g_signal_connect (dmr_cp,
                          "device-proxy-available",
                          G_CALLBACK (dmr_proxy_available_cb),
                          NULL);

        gssdp_resource_browser_set_active (GSSDP_RESOURCE_BROWSER (dms_cp),
                                           TRUE);
	gssdp_resource_browser_set_active (GSSDP_RESOURCE_BROWSER (dmr_cp),
                                           TRUE);

        /* Let context manager take care of the control point life cycle */
        gupnp_context_manager_manage_control_point (context_manager, dms_cp);
	gupnp_context_manager_manage_control_point (context_manager, dmr_cp);

        /* We don't need to keep our own references to the control points */
        g_object_unref (dms_cp);
	g_object_unref (dmr_cp);

}

static void
browse_data_free (BrowseData *data)
{

        g_free (data->id);
        g_object_unref (data->content_dir);
        g_slice_free (BrowseData, data);
}

static BrowseData *
browse_data_new (GUPnPServiceProxy *content_dir,
                 const char        *id,
                 guint32            starting_index)
{

        BrowseData *data;

        data = g_slice_new (BrowseData);
        data->content_dir = g_object_ref (content_dir);
        data->id = g_strdup (id);
        data->starting_index = starting_index;

        return data;
}

static void
on_didl_object_available (GUPnPDIDLLiteParser *parser,
                          GUPnPDIDLLiteObject *object,
                          gpointer             user_data)
{
        BrowseData   *browse_data;
	char *id;
	char *title;
	GList *resources;


	Container *c = (Container*)malloc(sizeof(Container));
	
	c->title = g_strdup(gupnp_didl_lite_object_get_title(object));
	c->parent_id = g_strdup(gupnp_didl_lite_object_get_parent_id(object));
	c->class = g_strdup(gupnp_didl_lite_object_get_upnp_class(object));

	resources = gupnp_didl_lite_object_get_resources(object);
	if(resources != NULL) {
		c->resource = (GUPnPDIDLLiteResource*)resources->data;
		puts(gupnp_didl_lite_resource_get_uri(c->resource));
	}
	id = g_strdup(gupnp_didl_lite_object_get_id(object));
	puts("----------");
	puts(c->title);
	puts(id);
	puts(c->parent_id);
	puts(c->class);
	
	puts("----------");
	
	g_hash_table_insert(browse_table, id, c);
	
        return;
}

static void
browse_cb (GUPnPServiceProxy       *content_dir,
           GUPnPServiceProxyAction *action,
           gpointer                 user_data)
{
	BrowseData *data;
        char       *didl_xml;
        guint32     number_returned;
        guint32     total_matches;
        GError     *error;

        data = (BrowseData *) user_data;
        didl_xml = NULL;
        error = NULL;

        gupnp_service_proxy_end_action (content_dir,
                                        action,
                                        &error,
                                        /* OUT args */
                                        "Result",
                                        G_TYPE_STRING,
                                        &didl_xml,
                                        "NumberReturned",
                                        G_TYPE_UINT,
                                        &number_returned,
                                        "TotalMatches",
                                        G_TYPE_UINT,
                                        &total_matches,
                                        NULL);
        if (didl_xml) {
                GUPnPDIDLLiteParser *parser;
                GError              *error;

                error = NULL;
                parser = gupnp_didl_lite_parser_new ();

                g_signal_connect (parser,
                                  "object-available",
                                  G_CALLBACK (on_didl_object_available),
                                  data);

                /* Only try to parse DIDL if server claims that there was a
                 * result */
                if (number_returned > 0)
                        if (!gupnp_didl_lite_parser_parse_didl (parser,
                                                                didl_xml,
                                                                &error)) {
                                g_warning ("Error while browsing %s: %s",
                                           data->id,
                                           error->message);
                                g_error_free (error);
                        }
		sem_post(&browse_sem);

                g_object_unref (parser);
                g_free (didl_xml);

	} else if (error) {
                GUPnPServiceInfo *info;

                info = GUPNP_SERVICE_INFO (content_dir);
                g_warning ("Failed to browse '%s': %s",
                           gupnp_service_info_get_location (info),
                           error->message);

                g_error_free (error);
		sem_post(&browse_sem);
        }

        browse_data_free (data);
}


static void
browse (GUPnPServiceProxy *content_dir,
        const char        *container_id,
        guint32            starting_index,
        guint32            requested_count)
{

        BrowseData *data;

        data = browse_data_new (content_dir,
                                container_id,
                                starting_index);

        gupnp_service_proxy_begin_action
		(content_dir,
		 "Browse",
		 browse_cb,
		 data,
		 /* IN args */
		 "ObjectID",
		 G_TYPE_STRING,
		 container_id,
		 "BrowseFlag",
		 G_TYPE_STRING,
		 "BrowseDirectChildren",
		 "Filter",
		 G_TYPE_STRING,
		 "@childCount",
		 "StartingIndex",
		 G_TYPE_UINT,
		 starting_index,
		 "RequestedCount",
		 G_TYPE_UINT,
		 requested_count,
		 "SortCriteria",
		 G_TYPE_STRING,
		 "",
		 NULL);
}

static BrowseMetadataData *
browse_metadata_data_new (MetadataFunc callback,
                          const char  *id,
                          gpointer     user_data)
{

        BrowseMetadataData *data;

        data = g_slice_new (BrowseMetadataData);
        data->callback = callback;
        data->id = g_strdup (id);
        data->user_data = user_data;

        return data;
}

static void
on_didl_item_available (GUPnPDIDLLiteParser *parser,
                        GUPnPDIDLLiteObject *object,
                        gpointer             user_data)
{

        GUPnPDIDLLiteResource **resource;
        char                   *sink_protocol_info;
        gboolean                lenient_mode;
	RendererData *data = NULL;

        resource = (GUPnPDIDLLiteResource **) user_data;


	data = (RendererData*)g_hash_table_lookup(renderer_table, current_renderer);
	sink_protocol_info = data->sink_protocol_info;
        
        *resource = gupnp_didl_lite_object_get_compat_resource
		(object,
		 sink_protocol_info,
		 lenient_mode);
        g_free (sink_protocol_info);

}


static GUPnPDIDLLiteResource *
find_compat_res_from_metadata (const char *metadata)
{

        GUPnPDIDLLiteParser   *parser;
        GUPnPDIDLLiteResource *resource;
        GError                *error;

        parser = gupnp_didl_lite_parser_new ();
        resource = NULL;
        error = NULL;

        g_signal_connect (parser,
                          "item-available",
                          G_CALLBACK (on_didl_item_available),
                          &resource);

        /* Assumption: metadata only contains a single didl object */
        gupnp_didl_lite_parser_parse_didl (parser, metadata, &error);
        if (error) {
                g_warning ("%s\n", error->message);

                g_error_free (error);
        }

        g_object_unref (parser);

        return resource;
}

static void
browse_metadata_data_free (BrowseMetadataData *data)
{

        g_free (data->id);
        g_slice_free (BrowseMetadataData, data);
}

static SetAVTransportURIData *
set_av_transport_uri_data_new (GCallback              callback,
                               GUPnPDIDLLiteResource *resource)
{
        printf("On %s function\n",__func__);
        SetAVTransportURIData *data;

        data = g_slice_new (SetAVTransportURIData);

        data->callback = callback;
        data->resource = resource; /* Steal the ref */

        return data;
}

static void
set_av_transport_uri_data_free (SetAVTransportURIData *data)
{
        printf("On %s function\n",__func__);
        g_object_unref (data->resource);
        g_slice_free (SetAVTransportURIData, data);
}


static void
set_av_transport_uri_cb (GUPnPServiceProxy       *av_transport,
                         GUPnPServiceProxyAction *action,
                         gpointer                 user_data)
{
        printf("On %s function\n",__func__);
        SetAVTransportURIData *data;
        GError                *error;

        data = (SetAVTransportURIData *) user_data;

        error = NULL;
        if (gupnp_service_proxy_end_action (av_transport,
                                            action,
                                            &error,
                                            NULL)) {
		sem_post(&play_sem);
		
	} else {
                const char *udn;

                udn = gupnp_service_info_get_udn
			(GUPNP_SERVICE_INFO (av_transport));

                g_warning ("Failed to set URI '%s' on %s: %s",
                           gupnp_didl_lite_resource_get_uri (data->resource),
                           udn,
                           error->message);

                g_error_free (error);
        }
	
        set_av_transport_uri_data_free (data);
//        g_object_unref (av_transport);
}



void set_av_transport_uri(const char *metadata, GUPnPServiceProxy *av_transport)
{
	GUPnPDIDLLiteResource *resource;
	const char *uri;
	SetAVTransportURIData *data;



	resource = find_compat_res_from_metadata (metadata);
	if (resource == NULL) {
		g_warning ("no compatible URI found.");
		
		return;
	}

	data = set_av_transport_uri_data_new (NULL, resource);
	uri = gupnp_didl_lite_resource_get_uri (resource);
	puts(uri);
//	puts(metadata);
	gupnp_service_proxy_begin_action (av_transport,
                                          "SetAVTransportURI",
                                          set_av_transport_uri_cb,
                                          data,
                                          "InstanceID",
                                          G_TYPE_UINT,
                                          0,
                                          "CurrentURI",
                                          G_TYPE_STRING,
                                          uri,
                                          "CurrentURIMetaData",
                                          G_TYPE_STRING,
                                          metadata,
                                          NULL);
}


static void
browse_metadata_cb (GUPnPServiceProxy       *content_dir,
                    GUPnPServiceProxyAction *action,
                    gpointer                 user_data)
{
	BrowseMetadataData *data;
        char               *metadata;
        GError             *error;
	char *udn;

	data = (BrowseMetadataData *) user_data;
        metadata = NULL;
        error = NULL;

        gupnp_service_proxy_end_action (content_dir,
                                        action,
                                        &error,
                                        /* OUT args */
                                        "Result",
                                        G_TYPE_STRING,
                                        &metadata,
                                        NULL);
        if (metadata) {
                
		RendererData *data = (RendererData*)g_hash_table_lookup(renderer_table, current_renderer);
		puts(data->friendly_name);

		set_av_transport_uri(metadata,(GUPnPServiceProxy*)data->av_transport);

                g_free (metadata);
        } else if (error) {
                g_warning ("Failed to get metadata for '%s': %s",
                           data->id,
                           error->message);

                g_error_free (error);
        }

        browse_metadata_data_free (data);
        g_object_unref (content_dir);
}


static void
browse_metadata (GUPnPServiceProxy *content_dir,
                 const char        *id)
{
 
        BrowseMetadataData *data;

        data = browse_metadata_data_new (NULL, id, NULL);

        gupnp_service_proxy_begin_action
		(g_object_ref (content_dir),
		 "Browse",
		 browse_metadata_cb,
		 data,
		 /* IN args */
		 "ObjectID",
		 G_TYPE_STRING,
		 data->id,
		 "BrowseFlag",
		 G_TYPE_STRING,
		 "BrowseMetadata",
		 "Filter",
		 G_TYPE_STRING,
		 "*",
		 "StartingIndex",
		 G_TYPE_UINT,
		 0,
		 "RequestedCount",
		 G_TYPE_UINT, 0,
		 "SortCriteria",
		 G_TYPE_STRING,
		 "",
		 NULL);
}

static void player_control(void);

static void
av_transport_action_cb (GUPnPServiceProxy       *av_transport,
                        GUPnPServiceProxyAction *action,
                        gpointer                 user_data)
{

        char *action_name;
        GError *error;

        action_name = (const char *) user_data;

        error = NULL;
        if (!gupnp_service_proxy_end_action (av_transport,
                                             action,
                                             &error,
                                             NULL)) {
                const char *udn;

                udn = gupnp_service_info_get_udn
			(GUPNP_SERVICE_INFO (av_transport));

                g_warning ("Failed to send action '%s' to '%s': %s",
                           action_name,
                           udn,
                           error->message);

                g_error_free (error);
        } else {
		if(!strcmp(action_name,"Play"))
			current_status = PLAYING;
	}
}

static void
g_value_free (gpointer data)
{
	g_value_unset ((GValue *) data);
	g_slice_free (GValue, data);
}


static GHashTable *
create_av_transport_args_hash (char **additional_args)
{
        printf("On %s function\n",__func__);
        GHashTable *args;
        GValue     *instance_id;
        gint        i;

        args = g_hash_table_new_full (g_str_hash,
                                      g_str_equal,
                                      NULL,
                                      g_value_free);

        instance_id = g_slice_alloc0 (sizeof (GValue));
        g_value_init (instance_id, G_TYPE_UINT);
        g_value_set_uint (instance_id, 0);

        g_hash_table_insert (args, "InstanceID", instance_id);

        if (additional_args != NULL) {
                for (i = 0; additional_args[i]; i += 2) {
                        GValue *value;

                        value = g_slice_alloc0 (sizeof (GValue));
                        g_value_init (value, G_TYPE_STRING);
                        g_value_set_string (value, additional_args[i + 1]);

                        g_hash_table_insert (args, additional_args[i], value);
                }
        }

        return args;
}



void
av_transport_send_action (char *action)
                          
{

	RendererData *r = (RendererData*)g_hash_table_lookup(renderer_table, current_renderer);

	if(!strcmp(action, "Play"))
		gupnp_service_proxy_begin_action ((GUPnPServiceProxy *)r->av_transport,
						  action,
						  av_transport_action_cb,
						  action,
						  "InstanceID", G_TYPE_UINT, 0,
						  "Speed", G_TYPE_UINT, 1,
						  NULL);
	else
		gupnp_service_proxy_begin_action ((GUPnPServiceProxy *)r->av_transport,
						  action,
						  av_transport_action_cb,
						  action,
						  "InstanceID", G_TYPE_UINT, 0,
						  NULL);
}

static void play_file (void)
{

        av_transport_send_action ("Play");
	
}

static void pause_file()
{
	av_transport_send_action ("Pause");
}

static void stop_file()
{
	av_transport_send_action ("Stop");
}



static void
get_position_info_cb (GUPnPServiceProxy       *av_transport,
                      GUPnPServiceProxyAction *action,
                      gpointer                 user_data)
{
	//      printf("On %s function\n",__func__);
        gchar       *position;
	gchar *duration;
        const gchar *udn;
        GError      *error;
	char curr_pos[64];
	static char track_duration[64];
	
	
        udn = gupnp_service_info_get_udn (GUPNP_SERVICE_INFO (av_transport));
        error = NULL;
        if (!gupnp_service_proxy_end_action (av_transport,
                                             action,
                                             &error,
                                             "AbsTime",
                                             G_TYPE_STRING,
                                             &position,
					     "TrackDuration",
					     G_TYPE_STRING,
					     &duration,
					     NULL)) {
                printf("Failed to get current media position"
                           "from media renderer '%s':%s\n",
                           udn,
                           error->message);
                g_error_free (error);
                goto return_point;
        }
	memset(curr_pos, 0, sizeof(curr_pos));
	strncpy(curr_pos, position, strlen(position) - 1);
	curr_pos[strlen(position)] = '\0';

	memset(track_duration, 0, sizeof(track_duration));
	strncpy(track_duration, duration, strlen(duration) - 1);
	track_duration[strlen(duration)] = '\0';
	


	printf("\r%s:%s", track_duration, curr_pos);

	fflush(stdout);
	
return_point:
	sem_post(&duration_sem);
	return;

//        g_object_unref (av_transport);
}

void *duration_update_func(void *ptr)
{
	printf("on %s\n",__func__);

	RendererData *r = (RendererData*)g_hash_table_lookup(renderer_table, current_renderer);

	printf("renderer name: %s\n",(const char*)r->friendly_name);
	while(1)
	{
		gupnp_service_proxy_begin_action ((GUPnPServiceProxy *)r->av_transport,
                                          "GetPositionInfo",
                                          get_position_info_cb,
                                          NULL,
                                          "InstanceID", G_TYPE_UINT, 0,
                                          NULL);

		sem_wait(&duration_sem);
		if(current_status == STOPPED)
			break;
		
	}
}


static void player_control()
{
	char user_input[256];
	
	GThread *duration_update;

	
	duration_update = g_thread_new("duration_update",(GThreadFunc)duration_update_func, NULL);
	sleep(1);
	while(1) {
		printf("\nEnter u/U to pause \nEnter p/P to play\nEnter s/S to stop\n");
		printf("> ");
		memset(user_input, 0, sizeof(user_input));
		fgets(user_input, sizeof(user_input), stdin);
		switch(user_input[0])
		{
		case 'u':
		case 'U':
			if(current_status == PLAYING) {
				pause_file();
				current_status = PAUSED;
			}
			break;
		case 'p':
		case 'P':
			if(current_status == PAUSED) {
				play_file();
				current_status = PLAYING;
			}
			break;
		case 's':
		case 'S':
			if(current_status == PLAYING || current_status == PAUSED) {
				stop_file();
				current_status = STOPPED;
			}
			break;
		default:
			printf("Enter valid input !!!\n");
		}
		
		if(current_status == STOPPED)
			break;
	}
			
}


void play(GUPnPServiceProxy *content_dir, char *id)
{
	char id_copy[256];
	int i = 1;
	RendererData *data = NULL;
	GHashTableIter iter;
	gpointer key, value;
	char user_input[256];
	char renderer_selected[256];
	
	strncpy(id_copy, id, strlen(id));
	id_copy[strlen(id)] = '\0';

	puts(id_copy);
select_renderer:	
	if(g_hash_table_size(renderer_table) != 0) {

		printf("Renderers list:\n");
		g_hash_table_iter_init(&iter, renderer_table);
		while(g_hash_table_iter_next(&iter, &key, &value))
		{

			data = (RendererData*)value;
			printf("%d . %s->%s\n",i, (const char*)data->friendly_name, (const char*)key);
			i++;
		}
		i = 1;
	}
	printf("Select Renderer: ");

	memset(user_input, 0, sizeof(user_input));
	memset(renderer_selected, 0, sizeof(renderer_selected));

	fgets(user_input, sizeof(user_input), stdin);
	strncpy(renderer_selected, user_input, (strlen(user_input) - 1));
	renderer_selected[strlen(user_input)] = '\0';
	if(NULL != g_hash_table_lookup(renderer_table, renderer_selected)) {
		Container *c;
		const char *uri;
		SetAVTransportURIData *data;

		strcpy(current_renderer, renderer_selected);
		c = (Container*)g_hash_table_lookup(browse_table, id_copy);
		if(c->resource != NULL) {
				data = set_av_transport_uri_data_new (NULL, c->resource);
				uri = gupnp_didl_lite_resource_get_uri (c->resource);
				puts(uri);
//	puts(metadata);
				RendererData *data = (RendererData*)g_hash_table_lookup(renderer_table, current_renderer);
				gupnp_service_proxy_begin_action ((GUPnPServiceProxy*)data->av_transport,
								  "SetAVTransportURI",
								  set_av_transport_uri_cb,
								  data,
								  "InstanceID",
								  G_TYPE_UINT,
								  0,
								  "CurrentURI",
								  G_TYPE_STRING,
								  uri,
								  "CurrentURIMetaData",
								  G_TYPE_STRING,
								  "",
								  NULL);
		} else {

			browse_metadata(g_object_ref(content_dir), id_copy);
		}
		sem_wait(&play_sem);
		play_file();
		player_control();
	} else {
		puts("Wrong input !! Enter valid renderer..");
		goto select_renderer;
	}
}


void *user_interaction(void *ptr)
{
	int i = 1;
	char user_input[256];
	char curr_server_udn[256];
	GHashTable *table = (GHashTable*)ptr;
	MediaServers *s = NULL;
	char curr_obj_id[256];

	while(1)
	{
		if(g_hash_table_size((GHashTable*)ptr) != 0)
		{
			GHashTableIter iter;
			gpointer key, value;

		refresh:
			g_hash_table_iter_init(&iter, table);
			while(g_hash_table_iter_next(&iter, &key, &value))
			{
				
				s = (MediaServers*)value;
				printf("%d . %s->%s\n",i, (const char*)s->friendly_name, gupnp_device_info_get_udn((GUPnPDeviceInfo*)s->info));
				i++;
			}
			i = 1;

			memset(user_input, 0, sizeof(user_input));
			memset(curr_server_udn, 0, sizeof(curr_server_udn));

			printf("Enter Server's udn for browse or r/R to refresh: ");
			fgets(user_input, sizeof(user_input), stdin);
			if(user_input[0] == 'r' || user_input[0] == 'R')
				goto refresh;
			strncpy(curr_server_udn, user_input, (strlen(user_input) - 1));
			curr_server_udn[strlen(user_input)] = '\0';

			if(g_hash_table_lookup(table, curr_server_udn) != NULL)
			{
				GHashTableIter iter;
				gpointer key, value;
				Container *c;
			browse_server:
				s = (MediaServers*)g_hash_table_lookup(table, curr_server_udn);
				browse((GUPnPServiceProxy*)s->content_dir, "0", 0, MAX_BROWSE);
				sem_wait(&browse_sem);

				g_hash_table_iter_init(&iter, browse_table);
				while(g_hash_table_iter_next(&iter, &key, &value))
				{
					c = (Container*)value;
					if(!strcmp(c->parent_id,"0")) {
						printf("  %d . %s->id:%s\n", i, (char*)c->title, (char*)key);
						i++;
					}
				}
				i = 1;

				printf("Enter the id to browse or r/R to previous menu: ");

				memset(user_input, 0, sizeof(user_input));
				memset(curr_obj_id, 0, sizeof(curr_obj_id));

				fgets(user_input, sizeof(user_input), stdin);
				if(user_input[0] == 'r' || user_input[0] == 'R') 
					goto refresh;
			
				strncpy(curr_obj_id, user_input, (strlen(user_input) - 1));
				curr_obj_id[strlen(user_input)] = '\0';
				
				puts(curr_obj_id);
			browse:
				if(g_hash_table_lookup(browse_table, curr_obj_id) != NULL)
				{
				
					browse((GUPnPServiceProxy*)s->content_dir, curr_obj_id, 0, MAX_BROWSE);
					sem_wait(&browse_sem);
					g_hash_table_iter_init(&iter, browse_table);
					while(g_hash_table_iter_next(&iter, &key, &value))
					{
						c = (Container*)value;
						if(!strcmp(curr_obj_id, c->parent_id)){
							printf("  %d . %s->id:%s\n", i, (char*)c->title, (char*)key);
							i++;
						}
					}
					
					i = 1;
					printf("Enter the id to browse/play or r/R to previous menu: ");
					memset(user_input, 0, sizeof(user_input));

					fgets(user_input, sizeof(user_input), stdin);

					if(user_input[0] == 'r' || user_input[0] == 'R') {
						c = (Container*)g_hash_table_lookup(browse_table, curr_obj_id);
						if(!strcmp(c->parent_id,"0"))
							goto browse_server;
						
						puts(c->parent_id);
						memset(curr_obj_id, 0, sizeof(curr_obj_id));
						
						strncpy(curr_obj_id, c->parent_id, strlen(c->parent_id));
						curr_obj_id[strlen(c->parent_id)] = '\0';

						goto browse;
					}
					
					puts(c->class);
					memset(curr_obj_id, 0, sizeof(curr_obj_id));
					
					strncpy(curr_obj_id, user_input, strlen(user_input) - 1);
					curr_obj_id[strlen(user_input)] = '\0';
					if(g_hash_table_lookup(browse_table, curr_obj_id) == NULL) {
						memset(curr_obj_id, 0, sizeof(curr_obj_id));

                                                strncpy(curr_obj_id, c->parent_id, strlen(c->parent_id));
						curr_obj_id[strlen(c->parent_id)] = '\0';
						goto browse;
					}

					c = (Container*)g_hash_table_lookup(browse_table, curr_obj_id);
					if(strncmp(c->class, OBJECT_CLASS_CONTAINER, strlen(OBJECT_CLASS_CONTAINER)))
					{
						
						play(((GUPnPServiceProxy*)s->content_dir), curr_obj_id);
						memset(curr_obj_id, 0, sizeof(curr_obj_id));

                                                strncpy(curr_obj_id, c->parent_id, strlen(c->parent_id));
						curr_obj_id[strlen(c->parent_id)] = '\0';
					}
						
					goto browse;
						
				} else {
					goto refresh;
				}
				
			} else {

				goto refresh;
			}
			
		}
	}
}

int main(int argc, char **argv)
{
	GMainLoop *loop;
	GError *err = NULL;
	GThread *user_thread;

#if !GLIB_CHECK_VERSION(2, 35, 0)
        g_type_init ();
#endif
	server_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	browse_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	renderer_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	sem_init(&browse_sem, 0, 0);
	sem_init(&play_sem, 0, 0);
	sem_init(&duration_sem, 0, 0);

	loop = g_main_loop_new(NULL, FALSE);
        context_manager = gupnp_context_manager_create (upnp_port);
        g_assert (context_manager != NULL);

        g_signal_connect (context_manager,
                          "context-available",
                          G_CALLBACK (on_context_available),
                          NULL);

	user_thread = g_thread_new("user_thread",(GThreadFunc)user_interaction, (void *)server_table);
	
	g_main_loop_run(loop);

        return 0;
}
