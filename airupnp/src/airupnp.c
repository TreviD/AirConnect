/*
 * AirUPnP - AirPlay to uPNP gateway
 *
 *	(c) Philippe 2015-2017, philippe_44@outlook.com
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
 */

#include <math.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#if WIN
#include <process.h>
#endif

#include "platform.h"
#include "airupnp.h"
#include "upnpdebug.h"
#include "upnptools.h"
#include "util.h"
#include "avt_util.h"
#include "config_upnp.h"
#include "mr_util.h"
#include "log_util.h"

#define VERSION "v0.1.6.1"" ("__DATE__" @ "__TIME__")"

#define	AV_TRANSPORT 			"urn:schemas-upnp-org:service:AVTransport"
#define	RENDERING_CTRL 			"urn:schemas-upnp-org:service:RenderingControl"
#define	CONNECTION_MGR 			"urn:schemas-upnp-org:service:ConnectionManager"
#define TOPOLOGY				"urn:schemas-upnp-org:service:ZoneGroupTopology"
#define GROUP_RENDERING_CTRL	"urn:schemas-upnp-org:service:GroupRenderingControl"

/*----------------------------------------------------------------------------*/
/* globals initialized */
/*----------------------------------------------------------------------------*/
#if LINUX || FREEBSD || SUNOS
bool				glDaemonize = false;
#endif
bool				glInteractive = true;
char				*glLogFile;
s32_t				glLogLimit = -1;
static char			*glPidFile = NULL;
static char			*glSaveConfigFile = NULL;
bool				glAutoSaveConfigFile = false;
bool				glGracefullShutdown = true;
bool				glDrift = false;

log_level	main_loglevel = lINFO;
log_level	raop_loglevel = lINFO;
log_level	util_loglevel = lWARN;
log_level	upnp_loglevel = lINFO;

tMRConfig			glMRConfig = {
							"-3",      	// StreamLength
							true,		// Enabled
							"",      	// Name
							true,		// SendMetaData
							false,		// SendCoverArt
							100,		// MaxVolume
							3,			// UPnPRemoveCount
							"flac",	    // Codec
							"",			// RTP:HTTP Latency (0 = use AirPlay requested)
							{0, 0, 0, 0, 0, 0 } // MAC
					};

/*----------------------------------------------------------------------------*/
/* globals */
/*----------------------------------------------------------------------------*/
static pthread_t 	glMainThread;
char				glUPnPSocket[128] = "?";
unsigned int 		glPort;
UpnpClient_Handle 	glControlPointHandle;
void				*glConfigID = NULL;
char				glConfigName[_STR_LEN_] = "./config.xml";
u32_t				glScanInterval = SCAN_INTERVAL;
u32_t				glScanTimeout = SCAN_TIMEOUT;
struct sMR			glMRDevices[MAX_RENDERERS];

/*----------------------------------------------------------------------------*/
/* consts or pseudo-const*/
/*----------------------------------------------------------------------------*/
static const char 	MEDIA_RENDERER[] 	= "urn:schemas-upnp-org:device:MediaRenderer:1";

static const struct cSearchedSRV_s
{
 char 	name[RESOURCE_LENGTH];
 int	idx;
 u32_t  TimeOut;
} cSearchedSRV[NB_SRV] = {	{AV_TRANSPORT, AVT_SRV_IDX, 0},
						{RENDERING_CTRL, REND_SRV_IDX, 30},
						{CONNECTION_MGR, CNX_MGR_IDX, 0},
						{TOPOLOGY, TOPOLOGY_IDX, 0},
						{GROUP_RENDERING_CTRL, GRP_REND_SRV_IDX, 0},
				   };

/*----------------------------------------------------------------------------*/
/* locals */
/*----------------------------------------------------------------------------*/
static log_level*		loglevel = &main_loglevel;
pthread_t				glUpdateMRThread;
static bool				glMainRunning = true;
static enum	{DISCOVERY_STOPPED, DISCOVERY_PENDING, DISCOVERY_UPDATING} glDiscoveryRunning = DISCOVERY_STOPPED;
static struct in_addr 	glHost;
static char				glHostName[_STR_LEN_];
static struct mdnsd*	glmDNSServer = NULL;
static char*			glExcluded = NULL;
static pthread_mutex_t	glEventMutex;
static struct sLocList {
	char 			*Location;
	struct sLocList *Next;
} *glMRFoundList = NULL;


static char usage[] =
			VERSION "\n"
		   "See -t for license terms\n"
		   "Usage: [options]\n"
		   "  -b <server>[:<port>]\tnetwork interface and UPnP port to use \n"
		   "  -x <config file>\tread config from file (default is ./config.xml)\n"
		   "  -i <config file>\tdiscover players, save <config file> and exit\n"
		   "  -I \t\t\tauto save config at every network scan\n"
		   "  -l <[rtp][:http]>\tset RTP and HTTP latency (ms)\n"
   		   "  -r \t\t\tlet timing reference drift (no click)\n"
		   "  -f <logfile>\t\twrite debug to logfile\n"
		   "  -p <pid file>\t\twrite PID in file\n"
		   "  -m <name1,name2...>\texclude from search devices whose model name contains name1 or name 2 ...\n"
		   "  -d <log>=<level>\tSet logging level, logs: all|raop|main|util|upnp, level: error|warn|info|debug|sdebug\n"

#if LINUX || FREEBSD
		   "  -z \t\t\tDaemonize\n"
#endif
		   "  -Z \t\t\tNOT interactive\n"
		   "  -k \t\t\tImmediate exit on SIGQUIT and SIGTERM\n"
		   "  -t \t\t\tLicense terms\n"
		   "\n"
		   "Build options:"
#if LINUX
		   " LINUX"
#endif
#if WIN
		   " WIN"
#endif
#if OSX
		   " OSX"
#endif
#if FREEBSD
		   " FREEBSD"
#endif
#if EVENTFD
		   " EVENTFD"
#endif
#if SELFPIPE
		   " SELFPIPE"
#endif
#if WINEVENT
		   " WINEVENT"
#endif
		   "\n\n";

static char license[] =
		   "This program is free software: you can redistribute it and/or modify\n"
		   "it under the terms of the GNU General Public License as published by\n"
		   "the Free Software Foundation, either version 3 of the License, or\n"
		   "(at your option) any later version.\n\n"
		   "This program is distributed in the hope that it will be useful,\n"
		   "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
		   "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
		   "GNU General Public License for more details.\n\n"
		   "You should have received a copy of the GNU General Public License\n"
		   "along with this program.  If not, see <http://www.gnu.org/licenses/>.\n\n"
	;


/*----------------------------------------------------------------------------*/
/* prototypes */
/*----------------------------------------------------------------------------*/
static 	void*	MRThread(void *args);
static 	void*	UpdateMRThread(void *args);
static 	bool 	AddMRDevice(struct sMR *Device, char * UDN, IXML_Document *DescDoc,	const char *location);
bool 			isExcluded(char *Model);

void callback(void *owner, raop_event_t event, void *param)
{
	struct sMR *device = (struct sMR*) owner;

	// need to use a mutex as PLAY comes from another thread than the others
	pthread_mutex_lock(&device->Mutex);

	switch (event) {
		case RAOP_STREAM:
			// a PLAY will come later, so we'll do the load at that time
			LOG_INFO("[%p]: Stream", device);
			device->RaopState = event;
			break;
		case RAOP_STOP:
			// this is TEARDOWN, so far there is always a FLUSH before
			LOG_INFO("[%p]: Stop", device);
			if (device->RaopState == RAOP_PLAY) {
				AVTStop(device);
				device->ExpectStop = true;
			}
			device->RaopState = event;
			NFREE(device->CurrentURI);
			break;
		case RAOP_FLUSH:
			LOG_INFO("[%p]: Flush", device);
			AVTStop(device);
			device->RaopState = event;
			device->ExpectStop = true;
			NFREE(device->CurrentURI);
			break;
		case RAOP_PLAY:
			if (device->RaopState != RAOP_PLAY) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
				asprintf(&device->CurrentURI, "http://%s:%u/stream.%s", inet_ntoa(glHost),
								*((short unsigned*) param),
								device->Config.Codec);
#pragma GCC diagnostic pop
				AVTSetURI(device);
				NFREE(device->CurrentURI);
			}

			AVTPlay(device);

			CtrlSetVolume(device, device->Volume, device->seqN++);
			device->RaopState = event;
			break;
		case RAOP_VOLUME: {
			device->Volume = *((double*) param) * device->Config.MaxVolume;
			CtrlSetVolume(device, device->Volume, device->seqN++);
			LOG_INFO("[%p]: Volume[0..100] %d", device, device->Volume);
			break;
		}
		default:
			break;
	}

	pthread_mutex_unlock(&device->Mutex);
}


/*----------------------------------------------------------------------------*/
bool ProcessQueue(struct sMR *Device)
{
	struct sService *Service = &Device->Service[AVT_SRV_IDX];
	tAction *Action;
	int rc = 0;

	Device->WaitCookie = 0;
	if ((Action = QueueExtract(&Device->ActionQueue)) == NULL) return false;

	Device->WaitCookie = Device->seqN++;
	rc = UpnpSendActionAsync(glControlPointHandle, Service->ControlURL, Service->Type,
							 NULL, Action->ActionNode, CallbackActionHandler, Device->WaitCookie);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("Error in queued UpnpSendActionAsync -- %d", rc);
	}

	ixmlDocument_free(Action->ActionNode);
	free(Action);

	return (rc == 0);
}


/*----------------------------------------------------------------------------*/
void HandleStateEvent(struct Upnp_Event *Event, void *Cookie)
{
	struct sMR *Device;
	IXML_Document *VarDoc = Event->ChangedVariables;
	char  *r = NULL;
	char  *LastChange = NULL;

	Device = SID2Device(Event->Sid);

	if (!Device || !Device->Raop) {
		LOG_SDEBUG("no RAOP device (yet) for %s", Event->Sid);
		return;
	}

	if (Device->Magic != MAGIC) {
		LOG_ERROR("[%p]: Wrong magic ", Device);
		return;
	}

	LastChange = XMLGetFirstDocumentItem(VarDoc, "LastChange");
	LOG_SDEBUG("Data event %s %u %s", Event->Sid, Event->EventKey, LastChange);
	if (!LastChange) return;
	NFREE(LastChange);

	// Feedback volume to AirPlay controller
	r = XMLGetChangeItem(VarDoc, "Volume", "channel", "Master", "val");
	if (r) {
		double Volume;
		int GroupVolume = GetGroupVolume(Device);

		Volume = (GroupVolume > 0) ? GroupVolume : atof(r);

		if ((int) Volume != Device->Volume) {
			LOG_INFO("[%p]: UPnP Volume local change %d", Device, (int) Volume);
			Volume /=  Device->Config.MaxVolume;
			raop_notify(Device->Raop, RAOP_VOLUME, &Volume);
		}
	}

	NFREE(r);
}


/*----------------------------------------------------------------------------*/
int CallbackActionHandler(Upnp_EventType EventType, void *Event, void *Cookie)
{
	LOG_SDEBUG("action: %i [%s] [%p]", EventType, uPNPEvent2String(EventType), Cookie);

	switch ( EventType ) {
		case UPNP_CONTROL_ACTION_COMPLETE: 	{
			struct Upnp_Action_Complete *Action = (struct Upnp_Action_Complete *)Event;
			struct sMR *p;
			char   *r;

			p = CURL2Device(Action->CtrlUrl);
			if (!p) break;

			LOG_SDEBUG("[%p]: ac %i %s (cookie %p)", p, EventType, Action->CtrlUrl, Cookie);

			// If waited action has been completed, proceed to next one if any
			if (p->WaitCookie)  {
				const char *Resp = XMLGetLocalName(Action->ActionResult, 1);

				LOG_DEBUG("[%p]: Waited action %s", p, Resp ? Resp : "<none>");

				// discard everything else except waiting action
				if (Cookie != p->WaitCookie) break;

				pthread_mutex_lock(&p->Mutex);

				p->StartCookie = p->WaitCookie;
				ProcessQueue(p);

				/*
				when certain waited action has been completed, the state need
				to be re-acquired because a 'stop' state might be missed when
				(eg) repositionning where two consecutive status update will
				give 'playing', the 'stop' in the middle being unseen
				*/
				if (Resp && (!strcasecmp(Resp, "StopResponse") ||
							 !strcasecmp(Resp, "PlayResponse") ||
							 !strcasecmp(Resp, "PauseResponse"))) {
					p->State = UNKNOWN;
				}

				pthread_mutex_unlock(&p->Mutex);
				break;
			}

			// don't proceed anything that is too old
			if (Cookie < p->StartCookie) break;

			// transport state response
			if ((r = XMLGetFirstDocumentItem(Action->ActionResult, "CurrentTransportState")) != NULL) {
				if (!strcmp(r, "TRANSITIONING") && p->State != TRANSITIONING) {
					p->State = TRANSITIONING;
					LOG_INFO("[%p]: uPNP transition", p);
				} else if (!strcmp(r, "STOPPED") && p->State != STOPPED) {
					if (p->RaopState == RAOP_PLAY && !p->ExpectStop) raop_notify(p->Raop, RAOP_STOP, NULL);
					p->State = STOPPED;
					p->ExpectStop = false;
					LOG_INFO("[%p]: uPNP stopped", p);
				} else if (!strcmp(r, "PLAYING") && (p->State != PLAYING)) {
					p->State = PLAYING;
					if (p->RaopState != RAOP_PLAY) raop_notify(p->Raop, RAOP_PLAY, NULL);
					LOG_INFO("[%p]: uPNP playing", p);
				} else if (!strcmp(r, "PAUSED_PLAYBACK") && p->State != PAUSED) {
					p->State = PAUSED;
					if (p->RaopState == RAOP_PLAY) raop_notify(p->Raop, RAOP_PAUSE, NULL);
					LOG_INFO("[%p]: uPNP pause", p);
				}
			}

			NFREE(r);

			LOG_SDEBUG("Action complete : %i (cookie %p)", EventType, Cookie);

			if (Action->ErrCode != UPNP_E_SUCCESS) {
				p->ErrorCount++;
				LOG_ERROR("Error in action callback -- %d (cookie %p)",	Action->ErrCode, Cookie);
			} else p->ErrorCount = 0;
			break;
		}
		default:
			break;
	}

	Cookie = Cookie;
	return 0;
}


/*----------------------------------------------------------------------------*/
int CallbackEventHandler(Upnp_EventType EventType, void *Event, void *Cookie)
{
	LOG_SDEBUG("event: %i [%s] [%p]", EventType, uPNPEvent2String(EventType), Cookie);

	switch ( EventType ) {
		case UPNP_DISCOVERY_ADVERTISEMENT_ALIVE:
		break;
		case UPNP_DISCOVERY_SEARCH_RESULT: {
			struct Upnp_Discovery *d_event = (struct Upnp_Discovery *) Event;

			LOG_DEBUG("Answer to uPNP search %s", d_event->Location);
			if (d_event->ErrCode != UPNP_E_SUCCESS) {
				LOG_SDEBUG("Error in Discovery Callback -- %d", d_event->ErrCode);
				break;
			}

			// this must *not* bet interrupted by the SEARCH_TIMEOUT event
			pthread_mutex_lock(&glEventMutex);

			if (glMainRunning && glDiscoveryRunning != DISCOVERY_UPDATING) {
				struct sLocList **p, *prev = NULL;

				p = &glMRFoundList;
				while (*p) {
					prev = *p;
					p = &((*p)->Next);
				}
				(*p) = (struct sLocList*) malloc(sizeof (struct sLocList));
				(*p)->Location = strdup(d_event->Location);
				(*p)->Next = NULL;
				if (prev) prev->Next = *p;
			}

			pthread_mutex_unlock(&glEventMutex);

			break;
		}
		case UPNP_DISCOVERY_SEARCH_TIMEOUT:	{
			pthread_attr_t attr;

			if (!glMainRunning) break;

			// in case we are interrupting SEARCH_RESULT
			pthread_mutex_lock(&glEventMutex);
			glDiscoveryRunning = DISCOVERY_UPDATING;
			pthread_mutex_unlock(&glEventMutex);

			pthread_attr_init(&attr);
			pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + 32*1024);
			pthread_create(&glUpdateMRThread, &attr, &UpdateMRThread, NULL);
			pthread_detach(glUpdateMRThread);
			pthread_attr_destroy(&attr);
			break;
		}
		case UPNP_EVENT_RECEIVED:
			HandleStateEvent(Event, Cookie);
			break;
		case UPNP_CONTROL_GET_VAR_COMPLETE:
			LOG_ERROR("Unexpected GetVarComplete", NULL);
			break;
		case UPNP_DISCOVERY_ADVERTISEMENT_BYEBYE: {
			struct Upnp_Discovery *d_event = (struct Upnp_Discovery *) Event;
			struct sMR *p = UDN2Device(d_event->DeviceId);

			if (!p) break;

			pthread_mutex_lock(&p->Mutex);

			if (!*d_event->ServiceType) {
				p->Eventing = EVT_BYEBYE;
				LOG_INFO("[%p]: Player BYE-BYE", p);
			}

			pthread_mutex_unlock(&p->Mutex);

			break;
		}
		case UPNP_EVENT_AUTORENEWAL_FAILED: {
			struct Upnp_Event_Subscribe *d_Event = (struct Upnp_Event_Subscribe *)Event;
			struct sMR *p = SID2Device(d_Event->Sid);
			int i, ret = UPNP_E_SUCCESS;

			if (!p) break;

			pthread_mutex_lock(&p->Mutex);

			if (!p->InUse) break;

			// renew service subscribtion if needed
			for (i = 0; i < NB_SRV; i++) {
				struct sService *s = &p->Service[cSearchedSRV[i].idx];
				if (!strcmp(s->EventURL, d_Event->PublisherUrl)) {
					ret = UpnpSubscribe(glControlPointHandle, s->EventURL, &s->TimeOut, s->SID);
					break;
				}
			}

			if (ret != UPNP_E_SUCCESS) {
				LOG_WARN("[%p]: Auto-renewal failed, cannot re-subscribe", p);
				p->Eventing = EVT_FAILED;
			} else {
				LOG_WARN("[%p]: Auto-renewal failed, re-subscribe success", p);
			}

			pthread_mutex_unlock(&p->Mutex);

			break;
		}
		case UPNP_EVENT_SUBSCRIPTION_EXPIRED: {
			struct Upnp_Event_Subscribe *d_Event = (struct Upnp_Event_Subscribe *)Event;
			struct sMR *p;
			int i;

			p = SID2Device(d_Event->Sid);
			if (!p) break;

			// renew service subscribtion if needed
			for (i = 0; i < NB_SRV; i++) {
				struct sService *s = &p->Service[cSearchedSRV[i].idx];
				if (!strcmp(s->EventURL, d_Event->PublisherUrl) && ((s->TimeOut = cSearchedSRV[i].TimeOut) != 0)) {
					UpnpSubscribe(glControlPointHandle, s->EventURL, &s->TimeOut, s->SID);
					break;
                }
			}

			LOG_WARN("[%p]: Subscription manually renewal", p);
			break;
        }
		case UPNP_EVENT_RENEWAL_COMPLETE:
		case UPNP_EVENT_SUBSCRIBE_COMPLETE: {
			LOG_INFO("event: %i [%s] [%p]", EventType, uPNPEvent2String(EventType), Cookie);
			break;
		}
		case UPNP_EVENT_SUBSCRIPTION_REQUEST:
		case UPNP_CONTROL_ACTION_REQUEST:
		case UPNP_EVENT_UNSUBSCRIBE_COMPLETE:
		case UPNP_CONTROL_GET_VAR_REQUEST:
		case UPNP_CONTROL_ACTION_COMPLETE:
		break;
	}

	Cookie = Cookie;
	return 0;
}



/*----------------------------------------------------------------------------*/
#define TRACK_POLL  (1000)
#define STATE_POLL  (500)
#define MAX_ACTION_ERRORS (5)
static void *MRThread(void *args)
{
	int elapsed;
	unsigned last;
	struct sMR *p = (struct sMR*) args;

	last = gettime_ms();

	for (; p->Running;  usleep(250000)) {
		elapsed = gettime_ms() - last;
		p->StatePoll += elapsed;
		p->TrackPoll += elapsed;

		pthread_mutex_lock(&p->Mutex);

		/*
		should not request any status update if we are stopped, off or waiting
		for an action to be performed
		*/
		if ((p->RaopState != RAOP_PLAY && p->State == STOPPED) ||
			 p->ErrorCount > MAX_ACTION_ERRORS ||
			 p->WaitCookie) {
			pthread_mutex_unlock(&p->Mutex);
			last = gettime_ms();
			continue;
		}

		// get track position & CurrentURI
		if (p->TrackPoll > TRACK_POLL) {
			p->TrackPoll = 0;
			if (p->State != STOPPED && p->State != PAUSED) {
				AVTCallAction(p, "GetPositionInfo", p->seqN++);
			}
		}

		// do polling as event is broken in many uPNP devices

		if (p->StatePoll > STATE_POLL) {
			p->StatePoll = 0;
			AVTCallAction(p, "GetTransportInfo", p->seqN++);
		}

				// do polling as event is broken in many uPNP devices
		pthread_mutex_unlock(&p->Mutex);
		last = gettime_ms();
	}

	return NULL;
}


/*----------------------------------------------------------------------------*/
static bool RefreshTO(char *UDN)
{
	int i;

	for (i = 0; i < MAX_RENDERERS; i++) {
		if (glMRDevices[i].InUse && !strcmp(glMRDevices[i].UDN, UDN)) {
			int j;

			pthread_mutex_lock(&glMRDevices[i].Mutex);

			glMRDevices[i].TimeOut = false;
			glMRDevices[i].MissingCount = glMRDevices[i].Config.RemoveCount;
			glMRDevices[i].ErrorCount = 0;

			// remove a group device that is not currently playing
			if ( glMRDevices[i].RaopState != RAOP_PLAY && !isMaster(UDN, &glMRDevices[i].Service[TOPOLOGY_IDX], NULL) ) {
				glMRDevices[i].Eventing = EVT_BYEBYE;
			}

			// try to renew subscription if failed
			for (j = 0; glMRDevices[i].Eventing == EVT_FAILED && j < NB_SRV; j++) {
				struct sService *s = &glMRDevices[i].Service[j];
				if (s->TimeOut && UpnpSubscribe(glControlPointHandle, s->EventURL, &s->TimeOut, s->SID) != UPNP_E_SUCCESS) {
					LOG_INFO("[%p] service re-subscribe success", glMRDevices + i);
				}
			}

			pthread_mutex_unlock(&glMRDevices[i].Mutex);

			return true;
		}
	}
	return false;
}

/*----------------------------------------------------------------------------*/
static void *UpdateMRThread(void *args)
{
	struct sLocList *p;
	struct sMR *Device = NULL;
	int i, TimeStamp;

	LOG_DEBUG("Begin UPnP devices update", NULL);
	TimeStamp = gettime_ms();

	p = glMRFoundList;

	while (p && glMainRunning) {
		IXML_Document *DescDoc = NULL;
		char *UDN = NULL, *ModelName = NULL;
		int rc;

		rc = UpnpDownloadXmlDoc(p->Location, &DescDoc);
		if (rc != UPNP_E_SUCCESS) {
			LOG_DEBUG("Error obtaining description %s -- error = %d\n", p->Location, rc);
			if (DescDoc) ixmlDocument_free(DescDoc);
			p = p->Next;
			continue;
		}

		ModelName = XMLGetFirstDocumentItem(DescDoc, "modelName");
		UDN = XMLGetFirstDocumentItem(DescDoc, "UDN");
		if (!RefreshTO(UDN) && !isExcluded(ModelName)) {
			// new device so search a free spot.
			for (i = 0; i < MAX_RENDERERS && glMRDevices[i].InUse; i++);

			// no more room !
			if (i == MAX_RENDERERS) {
				LOG_ERROR("Too many uPNP devices", NULL);
				NFREE(UDN); NFREE(ModelName);
				break;
			}

			Device = &glMRDevices[i];
			if (AddMRDevice(Device, UDN, DescDoc, p->Location) && !glSaveConfigFile) {
				// create a new AirPlay
				Device->Raop = raop_create(glHost, glmDNSServer, Device->Config.Name,
										   "airupnp", Device->Config.mac, Device->Config.Codec,
										   glDrift, Device->Config.Latency, Device, callback);
				if (!Device->Raop) {
					LOG_ERROR("[%p]: cannot create RAOP instance (%s)", Device, Device->Config.Name);
					DelMRDevice(Device);
				}
			}
		}

		if (DescDoc) ixmlDocument_free(DescDoc);
		NFREE(UDN);	NFREE(ModelName);
		p = p->Next;
	}

	// free the list of discovered location URL's
	while (glMRFoundList) {
		p = glMRFoundList->Next;
		free(glMRFoundList->Location); free(glMRFoundList);
		glMRFoundList = p;
	}

	// then walk through the list of devices to remove missing ones
	for (i = 0; i < MAX_RENDERERS; i++) {
		Device = &glMRDevices[i];
		if (!Device->InUse) continue;
		if (Device->TimeOut && Device->MissingCount) Device->MissingCount--;
		if (Device->Eventing != EVT_BYEBYE && (Device->MissingCount || !Device->Config.RemoveCount)) continue;

		LOG_INFO("[%p]: removing renderer (%s)", Device, Device->Config.Name);
		raop_delete(Device->Raop);
		DelMRDevice(Device);
	}

	if (glAutoSaveConfigFile && !glSaveConfigFile) {
		LOG_DEBUG("Updating configuration %s", glConfigName);
		SaveConfig(glConfigName, glConfigID, false);
	}

	glDiscoveryRunning = DISCOVERY_STOPPED;

	LOG_DEBUG("End UPnP devices update %d", gettime_ms() - TimeStamp);

	return NULL;
}

/*----------------------------------------------------------------------------*/
static void *MainThread(void *args)
{
	unsigned last = gettime_ms();
	int ScanPoll = 0;

	while (glMainRunning) {
		int i, rc;
		int elapsed = gettime_ms() - last;

		// reset timeout and re-scan devices
		ScanPoll += elapsed;
		if (glScanInterval && ScanPoll > glScanInterval*1000 && glDiscoveryRunning == DISCOVERY_STOPPED) {
			ScanPoll = 0;

			glDiscoveryRunning = DISCOVERY_PENDING;
			for (i = 0; i < MAX_RENDERERS; i++) glMRDevices[i].TimeOut = true;

			// launch a new search for Media Render
			rc = UpnpSearchAsync(glControlPointHandle, glScanTimeout, MEDIA_RENDERER, NULL);
			if (UPNP_E_SUCCESS != rc) LOG_ERROR("Error sending search update%d", rc);
		}

		if (glLogFile && glLogLimit != - 1) {
			u32_t size = ftell(stderr);

			if (size > glLogLimit*1024*1024) {
				u32_t Sum, BufSize = 16384;
				u8_t *buf = malloc(BufSize);

				FILE *rlog = fopen(glLogFile, "rb");
				FILE *wlog = fopen(glLogFile, "r+b");
				LOG_DEBUG("Resizing log", NULL);
				for (Sum = 0, fseek(rlog, size - (glLogLimit*1024*1024) / 2, SEEK_SET);
					 (BufSize = fread(buf, 1, BufSize, rlog)) != 0;
					 Sum += BufSize, fwrite(buf, 1, BufSize, wlog));

				Sum = fresize(wlog, Sum);
				fclose(wlog);
				fclose(rlog);
				NFREE(buf);
				if (!freopen(glLogFile, "a", stderr)) {
					LOG_ERROR("re-open error while truncating log", NULL);
				}
            }
		}

		last = gettime_ms();
		sleep(1);
	}
	return NULL;
}


/*----------------------------------------------------------------------------*/
int uPNPSearchMediaRenderer(void)
{
	int rc;

	/* search for (Media Render and wait 15s */
	glDiscoveryRunning = DISCOVERY_PENDING;
	rc = UpnpSearchAsync(glControlPointHandle, SCAN_TIMEOUT, MEDIA_RENDERER, NULL);

	if (UPNP_E_SUCCESS != rc) {
		LOG_ERROR("Error sending uPNP search request%d", rc);
		return false;
	}
	return true;
}


/*----------------------------------------------------------------------------*/
static bool AddMRDevice(struct sMR *Device, char *UDN, IXML_Document *DescDoc, const char *location)
{
	char *deviceType = NULL;
	char *friendlyName = NULL;
	char *URLBase = NULL;
	char *presURL = NULL;
	char *manufacturer = NULL;
	int i;
	pthread_attr_t attr;
	unsigned long mac_size = 6;
	in_addr_t ip;

	// read parameters from default then config file
	memset(Device, 0, sizeof(struct sMR));
	memcpy(&Device->Config, &glMRConfig, sizeof(tMRConfig));
	LoadMRConfig(glConfigID, UDN, &Device->Config);
	if (!Device->Config.Enabled) return false;

	// Read key elements from description document
	deviceType = XMLGetFirstDocumentItem(DescDoc, "deviceType");
	friendlyName = XMLGetFirstDocumentItem(DescDoc, "friendlyName");
	if (!friendlyName || !*friendlyName) friendlyName = strdup(UDN);
	URLBase = XMLGetFirstDocumentItem(DescDoc, "URLBase");
	presURL = XMLGetFirstDocumentItem(DescDoc, "presentationURL");
	manufacturer = XMLGetFirstDocumentItem(DescDoc, "manufacturer");

	LOG_SDEBUG("UDN:\t%s\nDeviceType:\t%s\nFriendlyName:\t%s", UDN, deviceType, friendlyName);

	if (presURL) {
		char UsedPresURL[200] = "";
		UpnpResolveURL((URLBase ? URLBase : location), presURL, UsedPresURL);
		strcpy(Device->PresURL, UsedPresURL);
	}
	else strcpy(Device->PresURL, "");

	NFREE(deviceType);
	NFREE(URLBase);
	NFREE(presURL);

	/* find the different services */
	for (i = 0; i < NB_SRV; i++) {
		char *ServiceId = NULL, *ServiceType = NULL;
		char *EventURL = NULL, *ControlURL = NULL;

		strcpy(Device->Service[i].Id, "");
		if (XMLFindAndParseService(DescDoc, location, cSearchedSRV[i].name, &ServiceType, &ServiceId, &EventURL, &ControlURL)) {
			struct sService *s = &Device->Service[cSearchedSRV[i].idx];
			LOG_SDEBUG("\tservice [%s] %s %s, %s, %s", cSearchedSRV[i].name, ServiceType, ServiceId, EventURL, ControlURL);

			strncpy(s->Id, ServiceId, RESOURCE_LENGTH-1);
			strncpy(s->ControlURL, ControlURL, RESOURCE_LENGTH-1);
			strncpy(s->EventURL, EventURL, RESOURCE_LENGTH - 1);
			strncpy(s->Type, ServiceType, RESOURCE_LENGTH - 1);
			if ((s->TimeOut = cSearchedSRV[i].TimeOut) != 0)
				UpnpSubscribe(glControlPointHandle, s->EventURL, &s->TimeOut, s->SID);
		}

		NFREE(ServiceId);
		NFREE(ServiceType);
		NFREE(EventURL);
		NFREE(ControlURL);
	}

	if ( !isMaster(UDN, &Device->Service[TOPOLOGY_IDX], &friendlyName) ) {
		LOG_DEBUG("[%p] skipping Sonos slave %s", Device, friendlyName);
		NFREE(manufacturer);
		NFREE(friendlyName);
		return false;
	}

	LOG_INFO("[%p]: adding renderer (%s)", Device, friendlyName);

	pthread_mutex_init(&Device->Mutex, 0);
	Device->Magic = MAGIC;
	Device->TimeOut = false;
	Device->Eventing = EVT_ACTIVE;
	Device->MissingCount = Device->Config.RemoveCount;
	Device->Muted = true;	//assume device is muted
	Device->ErrorCount = 0;
	Device->Running = true;
	Device->InUse = true;
	Device->RaopState = RAOP_STOP;
	Device->State = STOPPED;
	Device->ExpectStop = false;
	Device->WaitCookie = Device->StartCookie = NULL;
	strcpy(Device->UDN, UDN);
	strcpy(Device->DescDocURL, location);
	if (!*Device->Config.Name) sprintf(Device->Config.Name, "%s+", friendlyName);
	strcpy(Device->Manufacturer, manufacturer);
	QueueInit(&Device->ActionQueue);
	Device->MetaData.title = strdup("Streaming from AirConnect");
	if (stristr(manufacturer, "Sonos")) Device->MetaData.duration = 1;

	if (!strcasecmp(Device->Config.Codec, "pcm"))
		Device->ProtoInfo = "http-get:*:audio/L16;rate=44100;channels=2:DLNA.ORG_PN=LPCM;DLNA.ORG_OP=00;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=05400000000000000000000000000000";
	else if (!strcasecmp(Device->Config.Codec, "wav"))
		Device->ProtoInfo = "http-get:*:audio/wav:DLNA.ORG_OP=00;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=05400000000000000000000000000000";
	else
		Device->ProtoInfo = "http-get:*:audio/flac:DLNA.ORG_OP=00;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=05400000000000000000000000000000";

	ip = ExtractIP(location);
	if (!memcmp(Device->Config.mac, "\0\0\0\0\0\0", mac_size)) {
		if (SendARP(ip, INADDR_ANY, Device->Config.mac, &mac_size)) {
			u32_t hash = hash32(UDN);

			LOG_ERROR("[%p]: cannot get mac %s, creating fake %x", Device, Device->Config.Name, hash);
			memcpy(Device->Config.mac + 2, &hash, 4);
		}
		memset(Device->Config.mac, 0xbb, 2);
	}

	MakeMacUnique(Device);

	NFREE(manufacturer);
	NFREE(friendlyName);

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + 32*1024);
	pthread_create(&Device->Thread, &attr, &MRThread, Device);
	pthread_attr_destroy(&attr);

	return true;
}


/*----------------------------------------------------------------------------*/
bool isExcluded(char *Model)
{
	char item[_STR_LEN_];
	char *p = glExcluded;

	if (!glExcluded) return false;

	do {
		sscanf(p, "%[^,]", item);
		if (stristr(Model, item)) return true;
		p += strlen(item);
	} while (*p++);

	return false;
}


/*----------------------------------------------------------------------------*/
static bool Start(void)
{
	char hostname[_STR_LEN_];
	int rc;
	char IP[16] = "";

	if (glScanInterval) {
		if (glScanInterval < SCAN_INTERVAL) glScanInterval = SCAN_INTERVAL;
		if (glScanTimeout < SCAN_TIMEOUT) glScanTimeout = SCAN_TIMEOUT;
		if (glScanTimeout > glScanInterval - SCAN_TIMEOUT) glScanTimeout = glScanInterval - SCAN_TIMEOUT;
	}

	memset(&glMRDevices, 0, sizeof(glMRDevices));

	UpnpSetLogLevel(UPNP_ALL);

	if (!strstr(glUPnPSocket, "?")) sscanf(glUPnPSocket, "%[^:]:%u", IP, &glPort);

	rc = UpnpInit(*IP ? IP : NULL, glPort);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("UPnP init failed: %d\n", rc);
		UpnpFinish();
		return false;
	}

	UpnpSetMaxContentLength(60000);

	if (!*IP) strcpy(IP, UpnpGetServerIpAddress());
	S_ADDR(glHost) = inet_addr(IP);
	gethostname(glHostName, _STR_LEN_);
	if (!glPort) glPort = UpnpGetServerPort();

	LOG_INFO("Binding to %s:%d", IP, glPort);

	rc = UpnpRegisterClient(CallbackEventHandler,
				&glControlPointHandle, &glControlPointHandle);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("Error registering ControlPoint: %d", rc);
		UpnpFinish();
		return false;
	}

	snprintf(hostname, _STR_LEN_, "%s.local", glHostName);
	if ((glmDNSServer = mdnsd_start(glHost)) == NULL) {
		UpnpFinish();
		return false;
	}

	/* start the main thread */
	pthread_mutex_init(&glEventMutex, 0);
	pthread_create(&glMainThread, NULL, &MainThread, NULL);

	mdnsd_set_hostname(glmDNSServer, hostname, glHost);

	uPNPSearchMediaRenderer();
	return true;
}


/*----------------------------------------------------------------------------*/
static bool Stop(void)
{
	struct sLocList *p;

	LOG_INFO("terminate search thread ...", NULL);
	while (glDiscoveryRunning == DISCOVERY_UPDATING) usleep(50000);

	LOG_INFO("flush renderers ...", NULL);
	FlushMRDevices();

	LOG_INFO("terminate main thread ...", NULL);
	pthread_join(glMainThread, NULL);
	pthread_mutex_destroy(&glEventMutex);

	UpnpUnRegisterClient(glControlPointHandle);
	UpnpFinish();

	while (glMRFoundList) {
		p = glMRFoundList->Next;
		free(glMRFoundList->Location); free(glMRFoundList);
		glMRFoundList = p;
	}

	// stop broadcasting devices
	mdnsd_stop(glmDNSServer);

	if (glConfigID) ixmlDocument_free(glConfigID);

	return true;
}

/*---------------------------------------------------------------------------*/
static void sighandler(int signum) {
	int i;

	glMainRunning = false;

	if (!glGracefullShutdown) {
		for (i = 0; i < MAX_RENDERERS; i++) {
			struct sMR *p = &glMRDevices[i];
			if (p->InUse && p->State == PLAYING) AVTStop(p);
		}
		LOG_INFO("forced exit", NULL);
		exit(0);
	}

	Stop();
	exit(0);
}


/*---------------------------------------------------------------------------*/
bool ParseArgs(int argc, char **argv) {
	char *optarg = NULL;
	int optind = 1;
	int i;

#define MAXCMDLINE 256
	char cmdline[MAXCMDLINE] = "";

	for (i = 0; i < argc && (strlen(argv[i]) + strlen(cmdline) + 2 < MAXCMDLINE); i++) {
		strcat(cmdline, argv[i]);
		strcat(cmdline, " ");
	}

	while (optind < argc && strlen(argv[optind]) >= 2 && argv[optind][0] == '-') {
		char *opt = argv[optind] + 1;
		if (strstr("bxdpifml", opt) && optind < argc - 1) {
			optarg = argv[optind + 1];
			optind += 2;
		} else if (strstr("tzZIkr", opt)) {
			optarg = NULL;
			optind += 1;
		}
		else {
			printf("%s", usage);
			return false;
		}

		switch (opt[0]) {
		case 'b':
			strcpy(glUPnPSocket, optarg);
			break;
		case 'f':
			glLogFile = optarg;
			break;
		case 'i':
			glSaveConfigFile = optarg;
			break;
		case 'I':
			glAutoSaveConfigFile = true;
			break;
		case 'p':
			glPidFile = optarg;
			break;
		case 'Z':
			glInteractive = false;
			break;
		case 'k':
			glGracefullShutdown = false;
			break;
		case 'r':
			glDrift = true;
			break;
		case 'm':
			glExcluded = optarg;
			break;
		case 'l':
			strcpy(glMRConfig.Latency, optarg);
			break;
#if LINUX || FREEBSD
		case 'z':
			glDaemonize = true;
			break;
#endif
		case 'd':
			{
				char *l = strtok(optarg, "=");
				char *v = strtok(NULL, "=");
				log_level new = lWARN;
				if (l && v) {
					if (!strcmp(v, "error"))  new = lERROR;
					if (!strcmp(v, "warn"))   new = lWARN;
					if (!strcmp(v, "info"))   new = lINFO;
					if (!strcmp(v, "debug"))  new = lDEBUG;
					if (!strcmp(v, "sdebug")) new = lSDEBUG;
					if (!strcmp(l, "all") || !strcmp(l, "raop")) 	  	raop_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "main"))     	main_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "util"))    	util_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "upnp"))    	upnp_loglevel = new;
				}
				else {
					printf("%s", usage);
					return false;
				}
			}
			break;
		case 't':
			printf("%s", license);
			return false;
		default:
			break;
		}
	}

	return true;
}


/*----------------------------------------------------------------------------*/
/*																			  */
/*----------------------------------------------------------------------------*/
int main(int argc, char *argv[])
{
	int i;
	char resp[20] = "";

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
#if defined(SIGQUIT)
	signal(SIGQUIT, sighandler);
#endif
#if defined(SIGHUP)
	signal(SIGHUP, sighandler);
#endif
#if defined(SIGPIPE)
	signal(SIGPIPE, SIG_IGN);
#endif

	// first try to find a config file on the command line
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-x")) {
			strcpy(glConfigName, argv[i+1]);
		}
	}

	// load config from xml file
	glConfigID = (void*) LoadConfig(glConfigName, &glMRConfig);

	// potentially overwrite with some cmdline parameters
	if (!ParseArgs(argc, argv)) exit(1);

	if (glLogFile) {
		if (!freopen(glLogFile, "a", stderr)) {
			fprintf(stderr, "error opening logfile %s: %s\n", glLogFile, strerror(errno));
		}
	}

	LOG_ERROR("Starting airupnp version: %s", VERSION);

	if (strtod("0.30", NULL) != 0.30) {
		LOG_ERROR("Wrong GLIBC version, use -static build", NULL);
		exit(1);
	}

	if (!glConfigID) {
		LOG_WARN("no config file, using defaults", NULL);
	}

#if LINUX || FREEBSD
	if (glDaemonize && !glSaveConfigFile) {
		if (daemon(1, glLogFile ? 1 : 0)) {
			fprintf(stderr, "error daemonizing: %s\n", strerror(errno));
		}
	}
#endif

	if (glPidFile) {
		FILE *pid_file;
		pid_file = fopen(glPidFile, "wb");
		if (pid_file) {
			fprintf(pid_file, "%d", getpid());
			fclose(pid_file);
		}
		else {
			LOG_ERROR("Cannot open PID file %s", (int) glPidFile);
		}
	}

	if (!Start()) {
		LOG_ERROR("Cannot start", NULL);
		exit(1);
	}

	if (glSaveConfigFile) {
		while (glDiscoveryRunning != DISCOVERY_STOPPED) sleep(1);
		SaveConfig(glSaveConfigFile, glConfigID, true);
	}

	while (strcmp(resp, "exit") && !glSaveConfigFile) {

#if LINUX || FREEBSD || SUNOS
		if (!glDaemonize && glInteractive)
			i = scanf("%s", resp);
		else
			pause();
#else
		if (glInteractive)
			i = scanf("%s", resp);
		else
#if OSX
			pause();
#else
			Sleep(INFINITE);
#endif
#endif

		if (!strcmp(resp, "raopdbg"))	{
			char level[20];
			i = scanf("%s", level);
			raop_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "maindbg"))	{
			char level[20];
			i = scanf("%s", level);
			main_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "utildbg"))	{
			char level[20];
			i = scanf("%s", level);
			util_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "upnpdbg"))	{
			char level[20];
			i = scanf("%s", level);
			upnp_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "save"))	{
			char name[128];
			i = scanf("%s", name);
			SaveConfig(name, glConfigID, true);
		}
	}

	glMainRunning = false;
	LOG_INFO("stopping squeelite devices ...", NULL);
	LOG_INFO("stopping UPnP devices ...", NULL);
	Stop();
	LOG_INFO("all done", NULL);

	return true;
}

