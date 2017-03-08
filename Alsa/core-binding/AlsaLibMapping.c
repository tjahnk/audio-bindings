/*
 * AlsaLibMapping -- provide low level interface with ALSA lib (extracted from alsa-json-gateway code)
 * Copyright (C) 2015,2016,2017, Fulup Ar Foll fulup@iot.bzh
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.

   References:
   https://github.com/fulup-bzh/AlsaJsonGateway (original code)
   http://alsa-lib.sourcearchive.com/documentation/1.0.20/modules.html
   http://alsa-lib.sourcearchive.com/documentation/1.0.8/group__Control_gd48d44da8e3bfe150e928267008b8ff5.html
   http://alsa.opensrc.org/HowTo_access_a_mixer_control
   https://github.com/gch1p/alsa-volume-monitor/blob/master/main.c

*/

#define _GNU_SOURCE  // needed for vasprintf

#include <alsa/asoundlib.h>
#include "AlsaLibMapping.h"

#include <systemd/sd-event.h>

// generic structure to pass parsed query values
typedef struct {
  const char *devid;
  int  numid;
  int  quiet;  
} queryValuesT;

// generic sndctrl event handle hook to event callback when pooling
typedef struct {
    struct pollfd pfds;
    sd_event_source *src;
    snd_ctl_t *ctl;
    struct afb_event afbevt;
} evtHandleT;

typedef struct {
    int cardid;
    int ucount;
    evtHandleT *evtHandle;
} sndHandleT;




STATIC json_object *DB2StringJsonOject (long dB) {
    char label [20];
	if (dB < 0) {
		snprintf(label, sizeof(label), "-%li.%02lidB", -dB / 100, -dB % 100);
	} else {
		snprintf(label, sizeof(label), "%li.%02lidB", dB / 100, dB % 100);
	}

    // json function takes care of string copy
	return (json_object_new_string (label));
}


// Direct port from amixer TLV decode routine. This code is too complex for me.
// I hopefully did not break it when porting it.

STATIC json_object *decodeTlv(unsigned int *tlv, unsigned int tlv_size) {
    char label[20];
    unsigned int type = tlv[0];
    unsigned int size;
    unsigned int idx = 0;
    const char *chmap_type = NULL;
    json_object * decodeTlvJson = json_object_new_object();

    if (tlv_size < (unsigned int) (2 * sizeof (unsigned int))) {
        printf("TLV size error!\n");
        return NULL;
    }
    type = tlv[idx++];
    size = tlv[idx++];
    tlv_size -= (unsigned int) (2 * sizeof (unsigned int));
    if (size > tlv_size) {
        fprintf(stderr, "TLV size error (%i, %i, %i)!\n", type, size, tlv_size);
        return NULL;
    }
    switch (type) {

        case SND_CTL_TLVT_CONTAINER:
        {
            json_object * containerJson = json_object_new_array();

            size += (unsigned int) (sizeof (unsigned int) - 1);
            size /= (unsigned int) (sizeof (unsigned int));
            while (idx < size) {
                json_object *embedJson;

                if (tlv[idx + 1] > (size - idx) * sizeof (unsigned int)) {
                    fprintf(stderr, "TLV size error in compound!\n");
                    return NULL;
                }
                embedJson = decodeTlv(tlv + idx, tlv[idx + 1] + 8);
                json_object_array_add(containerJson, embedJson);
                idx += (unsigned int) (2 + (tlv[idx + 1] + sizeof (unsigned int) - 1) / sizeof (unsigned int));
            }
            json_object_object_add(decodeTlvJson, "container", containerJson);
            break;
        }

        case SND_CTL_TLVT_DB_SCALE:
        {
            json_object * dbscaleJson = json_object_new_object();

            if (size != 2 * sizeof (unsigned int)) {
                json_object * arrayJson = json_object_new_array();
                while (size > 0) {
                    snprintf(label, sizeof (label), "0x%08x,", tlv[idx++]);
                    json_object_array_add(arrayJson, json_object_new_string(label));
                    size -= (unsigned int) sizeof (unsigned int);
                }
                json_object_object_add(dbscaleJson, "array", arrayJson);
            } else {
                json_object_object_add(dbscaleJson, "min", DB2StringJsonOject((int) tlv[2]));
                json_object_object_add(dbscaleJson, "step", DB2StringJsonOject(tlv[3] & 0xffff));
                json_object_object_add(dbscaleJson, "mute", DB2StringJsonOject((tlv[3] >> 16) & 1));
            }
            json_object_object_add(decodeTlvJson, "dbscale", dbscaleJson);
            break;
        }

#ifdef SND_CTL_TLVT_DB_LINEAR
        case SND_CTL_TLVT_DB_LINEAR:
        {
            json_object * dbLinearJson = json_object_new_object();

            if (size != 2 * sizeof (unsigned int)) {
                json_object * arrayJson = json_object_new_array();
                while (size > 0) {
                    snprintf(label, sizeof (label), "0x%08x,", tlv[idx++]);
                    json_object_array_add(arrayJson, json_object_new_string(label));
                    size -= (unsigned int) sizeof (unsigned int);
                }
                json_object_object_add(dbLinearJson, "offset", arrayJson);
            } else {
                json_object_object_add(dbLinearJson, "min", DB2StringJsonOject((int) tlv[2]));
                json_object_object_add(dbLinearJson, "max", DB2StringJsonOject((int) tlv[3]));
            }
            json_object_object_add(decodeTlvJson, "dblinear", dbLinearJson);
            break;
        }
#endif

#ifdef SND_CTL_TLVT_DB_RANGE
        case SND_CTL_TLVT_DB_RANGE:
        {
            json_object *dbRangeJson = json_object_new_object();

            if ((size % (6 * sizeof (unsigned int))) != 0) {
                json_object *arrayJson = json_object_new_array();
                while (size > 0) {
                    snprintf(label, sizeof (label), "0x%08x,", tlv[idx++]);
                    json_object_array_add(arrayJson, json_object_new_string(label));
                    size -= (unsigned int) sizeof (unsigned int);
                }
                json_object_object_add(dbRangeJson, "dbrange", arrayJson);
                break;
            }
            while (size > 0) {
                json_object * embedJson = json_object_new_object();
                snprintf(label, sizeof (label), "%i,", tlv[idx++]);
                json_object_object_add(embedJson, "rangemin", json_object_new_string(label));
                snprintf(label, sizeof (label), "%i", tlv[idx++]);
                json_object_object_add(embedJson, "rangemax", json_object_new_string(label));
                embedJson = decodeTlv(tlv + idx, 4 * sizeof (unsigned int));
                json_object_object_add(embedJson, "tlv", embedJson);
                idx += 4;
                size -= (unsigned int) (6 * sizeof (unsigned int));
                json_object_array_add(dbRangeJson, embedJson);
            }
            json_object_object_add(decodeTlvJson, "dbrange", dbRangeJson);
            break;
        }
#endif
#ifdef SND_CTL_TLVT_DB_MINMAX
        case SND_CTL_TLVT_DB_MINMAX:
        case SND_CTL_TLVT_DB_MINMAX_MUTE:
        {
            json_object * dbMinMaxJson = json_object_new_object();

            if (size != 2 * sizeof (unsigned int)) {
                json_object * arrayJson = json_object_new_array();
                while (size > 0) {
                    snprintf(label, sizeof (label), "0x%08x,", tlv[idx++]);
                    json_object_array_add(arrayJson, json_object_new_string(label));
                    size -= (unsigned int) sizeof (unsigned int);
                }
                json_object_object_add(dbMinMaxJson, "array", arrayJson);

            } else {
                json_object_object_add(dbMinMaxJson, "min", DB2StringJsonOject((int) tlv[2]));
                json_object_object_add(dbMinMaxJson, "max", DB2StringJsonOject((int) tlv[3]));
            }

            if (type == SND_CTL_TLVT_DB_MINMAX_MUTE) {
                json_object_object_add(decodeTlvJson, "dbminmaxmute", dbMinMaxJson);
            } else {
                json_object_object_add(decodeTlvJson, "dbminmax", dbMinMaxJson);
            }
            break;
        }
#endif
#ifdef SND_CTL_TLVT_CHMAP_FIXED
        case SND_CTL_TLVT_CHMAP_FIXED:
            chmap_type = "fixed";
            /* Fall through */
        case SND_CTL_TLVT_CHMAP_VAR:
            if (!chmap_type)
                chmap_type = "variable";
            /* Fall through */
        case SND_CTL_TLVT_CHMAP_PAIRED:
            if (!chmap_type)
                chmap_type = "paired";

            json_object * chmapJson = json_object_new_object();
            json_object * arrayJson = json_object_new_array();

            while (size > 0) {
                snprintf(label, sizeof (label), "%s", snd_pcm_chmap_name(tlv[idx++]));
                size -= (unsigned int) sizeof (unsigned int);
                json_object_array_add(arrayJson, json_object_new_string(label));
            }
            json_object_object_add(chmapJson, chmap_type, arrayJson);
            json_object_object_add(decodeTlvJson, "chmap", chmapJson);
            break;
#endif
        default:
        {
            printf("unk-%i-", type);
            json_object * arrayJson = json_object_new_array();

            while (size > 0) {
                snprintf(label, sizeof (label), "0x%08x,", tlv[idx++]);
                size -= (unsigned int) sizeof (unsigned int);
                json_object_array_add(arrayJson, json_object_new_string(label));
            }
            break;
            json_object_object_add(decodeTlvJson, "unknown", arrayJson);
        }
    }

    return (decodeTlvJson);
}


// retreive info for one given card
STATIC  json_object* alsaCardProbe (const char *rqtSndId) {
    const char  *info, *name;
    const char *devid, *driver;
    json_object *sndcard;
    snd_ctl_t   *handle;
    snd_ctl_card_info_t *cardinfo;
    int err;


    snd_ctl_card_info_alloca(&cardinfo);

    if ((err = snd_ctl_open(&handle, rqtSndId, 0)) < 0) {
        INFO (afbIface, "SndCard [%s] Not Found", rqtSndId);
        return NULL;
    }

    if ((err = snd_ctl_card_info(handle, cardinfo)) < 0) {
        snd_ctl_close(handle);
        WARNING (afbIface, "SndCard [%s] info error: %s", rqtSndId, snd_strerror(err));
        return NULL;
    }

    // start a new json object to store card info
    sndcard = json_object_new_object();

    devid= snd_ctl_card_info_get_id(cardinfo);
    json_object_object_add (sndcard, "devid"  , json_object_new_string(devid));
    name =  snd_ctl_card_info_get_name(cardinfo);
    json_object_object_add (sndcard, "name", json_object_new_string (name));

    if (afbIface->verbosity > 1) {
        json_object_object_add (sndcard, "devid", json_object_new_string(rqtSndId));
        driver= snd_ctl_card_info_get_driver(cardinfo);
        json_object_object_add (sndcard, "driver"  , json_object_new_string(driver));
        info  = strdup(snd_ctl_card_info_get_longname (cardinfo));
        json_object_object_add (sndcard, "info" , json_object_new_string (info));
        INFO (afbIface, "AJG: Soundcard Devid=%-5s Cardid=%-7s Name=%s\n", rqtSndId, devid, info);
    }

    // free card handle and return info
    snd_ctl_close(handle);    
    return (sndcard);
}

// Loop on every potential Sound card and register active one
PUBLIC void alsaGetInfo (struct afb_req request) {
    int  card;
    json_object *sndcard, *sndcards;
    char devid[32];
    
    const char *rqtSndId = afb_req_value(request, "devid");

    // if no specific card requested loop on all

    if (rqtSndId != NULL) {
        // only one card was requested let's probe it
        sndcard = alsaCardProbe (rqtSndId);
        if (sndcard != NULL) afb_req_success(request, sndcard, NULL);
        else afb_req_fail_f (request, "SndCard [%s] Not Found", rqtSndId);
        
    } else {
        // return an array of sndcard
        sndcards =json_object_new_array();

        // loop on potential card number
        for (card =0; card < 32; card++) {

            // build card devid and probe it
            snprintf (devid, sizeof(devid), "hw:%i", card);
            sndcard = alsaCardProbe (devid);

            // Alsa has hole within card list [ignore them]
            if (sndcard != NULL) {
                // add current sndcard to sndcards object
                json_object_array_add (sndcards, sndcard);
            }    
        }
        afb_req_success (request, sndcards, NULL);  
    }
}

// pack Alsa element's ACL into a JSON object
STATIC json_object *getControlAcl (snd_ctl_elem_info_t *info) {

    json_object * jsonAclCtl = json_object_new_object();

    json_object_object_add (jsonAclCtl, "read"   , json_object_new_boolean(snd_ctl_elem_info_is_readable(info)));
    json_object_object_add (jsonAclCtl, "write"  , json_object_new_boolean(snd_ctl_elem_info_is_writable(info)));
    json_object_object_add (jsonAclCtl, "inact"  , json_object_new_boolean(snd_ctl_elem_info_is_inactive(info)));
    json_object_object_add (jsonAclCtl, "volat"  , json_object_new_boolean(snd_ctl_elem_info_is_volatile(info)));
    json_object_object_add (jsonAclCtl, "lock"   , json_object_new_boolean(snd_ctl_elem_info_is_locked(info)));

    // if TLV is readable we insert its ACL
    if (!snd_ctl_elem_info_is_tlv_readable(info)) {
        json_object * jsonTlv = json_object_new_object();

        json_object_object_add (jsonTlv, "read"   , json_object_new_boolean(snd_ctl_elem_info_is_tlv_readable(info)));
        json_object_object_add (jsonTlv, "write"  , json_object_new_boolean(snd_ctl_elem_info_is_tlv_writable(info)));
        json_object_object_add (jsonTlv, "command", json_object_new_boolean(snd_ctl_elem_info_is_tlv_commandable(info)));

        json_object_object_add (jsonAclCtl, "tlv", jsonTlv);
    }
    return (jsonAclCtl);
}



// pack element from ALSA control into a JSON object
STATIC json_object * alsaGetSingleCtl (snd_hctl_elem_t *elem, snd_ctl_elem_info_t *info, queryValuesT *queryValues) {

    int err;
    json_object *jsonAlsaCtl,*jsonClassCtl;
    snd_ctl_elem_id_t *elemid;
    snd_ctl_elem_type_t elemtype;
    snd_ctl_elem_value_t *control;
    int count, idx;

    // allocate ram for ALSA elements
    snd_ctl_elem_value_alloca(&control);
    snd_ctl_elem_id_alloca   (&elemid);

    // get elemId out of elem
    snd_hctl_elem_get_id(elem, elemid);

    // when ctrlid is set, return only this ctrl
    if (queryValues->numid != -1 && queryValues->numid != snd_ctl_elem_id_get_numid(elemid)) return NULL;

    // build a json object out of element
    jsonAlsaCtl = json_object_new_object(); // http://alsa-lib.sourcearchive.com/documentation/1.0.24.1-3/group__Control_ga4e4f251147f558bc2ad044e836e449d9.html
    json_object_object_add (jsonAlsaCtl,"numid", json_object_new_int(snd_ctl_elem_id_get_numid (elemid)));
    if (queryValues->quiet < 2) json_object_object_add (jsonAlsaCtl,"name" , json_object_new_string(snd_ctl_elem_id_get_name (elemid)));
    if (queryValues->quiet < 1) json_object_object_add (jsonAlsaCtl,"iface", json_object_new_string(snd_ctl_elem_iface_name(snd_ctl_elem_id_get_interface(elemid))));
    if (queryValues->quiet < 3)json_object_object_add (jsonAlsaCtl,"actif", json_object_new_boolean(!snd_ctl_elem_info_is_inactive(info)));

    elemtype = snd_ctl_elem_info_get_type(info);

	// number item and value(s) within control.
	count = snd_ctl_elem_info_get_count (info);

	if (snd_ctl_elem_info_is_readable(info)) {
	    json_object *jsonValuesCtl = json_object_new_array();

		if ((err = snd_hctl_elem_read(elem, control)) < 0) {
		       json_object *jsonValuesCtl = json_object_new_object();
       		   json_object_object_add (jsonValuesCtl,"error", json_object_new_string(snd_strerror(err)));
		} else {
			for (idx = 0; idx < count; idx++) { // start from one in amixer.c !!!
				switch (elemtype) {
				case SND_CTL_ELEM_TYPE_BOOLEAN: {
					json_object_array_add (jsonValuesCtl, json_object_new_boolean (snd_ctl_elem_value_get_boolean(control, idx)));
					break;
					}
				case SND_CTL_ELEM_TYPE_INTEGER:
					json_object_array_add (jsonValuesCtl, json_object_new_int ((int)snd_ctl_elem_value_get_integer(control, idx)));
					break;
				case SND_CTL_ELEM_TYPE_INTEGER64:
					json_object_array_add (jsonValuesCtl, json_object_new_int64 (snd_ctl_elem_value_get_integer64(control, idx)));
					break;
				case SND_CTL_ELEM_TYPE_ENUMERATED:
					json_object_array_add (jsonValuesCtl, json_object_new_int (snd_ctl_elem_value_get_enumerated(control, idx)));
					break;
				case SND_CTL_ELEM_TYPE_BYTES:
					json_object_array_add (jsonValuesCtl, json_object_new_int ((int)snd_ctl_elem_value_get_byte(control, idx)));
					break;
				case SND_CTL_ELEM_TYPE_IEC958: {
					json_object *jsonIec958Ctl = json_object_new_object();
					snd_aes_iec958_t iec958;
					snd_ctl_elem_value_get_iec958(control, &iec958);

					json_object_object_add (jsonIec958Ctl,"AES0",json_object_new_int(iec958.status[0]));
					json_object_object_add (jsonIec958Ctl,"AES1",json_object_new_int(iec958.status[1]));
					json_object_object_add (jsonIec958Ctl,"AES2",json_object_new_int(iec958.status[2]));
					json_object_object_add (jsonIec958Ctl,"AES3",json_object_new_int(iec958.status[3]));
					json_object_array_add  (jsonValuesCtl, jsonIec958Ctl);
					break;
					}
				default:
					json_object_array_add (jsonValuesCtl, json_object_new_string ("?unknown?"));
					break;
				}
			}
		}
		json_object_object_add (jsonAlsaCtl,"value",jsonValuesCtl);
    }


    if (!queryValues->quiet) {  // in simple mode do not print usable values
        jsonClassCtl = json_object_new_object();
        json_object_object_add (jsonClassCtl,"type" , json_object_new_string(snd_ctl_elem_type_name(elemtype)));
		json_object_object_add (jsonClassCtl,"count", json_object_new_int(count));

		switch (elemtype) {
			case SND_CTL_ELEM_TYPE_INTEGER:
				json_object_object_add (jsonClassCtl,"min",  json_object_new_int((int)snd_ctl_elem_info_get_min(info)));
				json_object_object_add (jsonClassCtl,"max",  json_object_new_int((int)snd_ctl_elem_info_get_max(info)));
				json_object_object_add (jsonClassCtl,"step", json_object_new_int((int)snd_ctl_elem_info_get_step(info)));
				break;
			case SND_CTL_ELEM_TYPE_INTEGER64:
				json_object_object_add (jsonClassCtl,"min",  json_object_new_int64(snd_ctl_elem_info_get_min64(info)));
				json_object_object_add (jsonClassCtl,"max",  json_object_new_int64(snd_ctl_elem_info_get_max64(info)));
				json_object_object_add (jsonClassCtl,"step", json_object_new_int64(snd_ctl_elem_info_get_step64(info)));
				break;
			case SND_CTL_ELEM_TYPE_ENUMERATED: {
				unsigned int item, items = snd_ctl_elem_info_get_items(info);
				json_object *jsonEnum = json_object_new_array();

				for (item = 0; item < items; item++) {
					snd_ctl_elem_info_set_item(info, item);
					if ((err = snd_hctl_elem_info(elem, info)) >= 0) {
						json_object_array_add (jsonEnum, json_object_new_string(snd_ctl_elem_info_get_item_name(info)));
					}
				}
				json_object_object_add (jsonClassCtl, "enums",jsonEnum);
				break;
			}
			default: break; // ignore any unknown type
			}

		// add collected class info with associated ACLs
		json_object_object_add (jsonAlsaCtl,"ctrl", jsonClassCtl);
		json_object_object_add (jsonAlsaCtl,"acl"  , getControlAcl (info));

		// check for tlv [direct port from amixer.c]
		if (snd_ctl_elem_info_is_tlv_readable(info)) {
				unsigned int *tlv;
				tlv = malloc(4096);
				if ((err = snd_hctl_elem_tlv_read(elem, tlv, 4096)) < 0) {
					fprintf (stderr, "Control %s element TLV read error\n", snd_strerror(err));
					free(tlv);
				} else {
					json_object_object_add (jsonAlsaCtl,"tlv", decodeTlv (tlv, 4096));
		   }
		}
   }
   return (jsonAlsaCtl);
}


PUBLIC void alsaGetCtl(struct afb_req request) {
    int err = 0;
    snd_hctl_t *handle;
    snd_hctl_elem_t *elem;
    snd_ctl_elem_info_t *info;
    json_object *sndctrls, *control;
    queryValuesT queryValues;

    // get API params
    queryValues.devid = afb_req_value(request, "devid");
    
    const char *rqtNumid = afb_req_value(request, "numid");
    if (!rqtNumid) queryValues.numid=-1;
    else if (rqtNumid && ! sscanf (rqtNumid, "%d", &queryValues.numid)) {
        json_object *query = afb_req_json(request);
        
        afb_req_fail_f (request, "Query=%s NumID not integer &numid=%s&", json_object_to_json_string(query), rqtNumid);
        goto ExitOnError;
    };
    
    const char *rqtQuiet = afb_req_value(request, "quiet");
    if (!rqtQuiet) queryValues.quiet=99; // default super quiet
    else if (rqtQuiet && ! sscanf (rqtQuiet, "%d", &queryValues.quiet)) {
        json_object *query = afb_req_json(request);
        
        afb_req_fail_f (request, "Query=%s NumID not integer &numid=%s&", json_object_to_json_string(query), rqtQuiet);
        goto ExitOnError;
    };
    
    // Open sound we use Alsa high level API like amixer.c
    if (!queryValues.devid || (err = snd_hctl_open(&handle, queryValues.devid, 0)) < 0) {
        afb_req_fail_f (request, "alsaGetControl devid=[%s] open fail error=%s\n", queryValues.devid, snd_strerror(err));
        goto ExitOnError;
    }

    if ((err = snd_hctl_load(handle)) < 0) {
        afb_req_fail_f (request, "alsaGetControl devid=[%s] load fail error=%s\n", queryValues.devid, snd_strerror(err));
        goto ExitOnError;
    }

    // allocate ram for ALSA elements
    snd_ctl_elem_info_alloca(&info);

    // create an json array to hold all sndcard response
    sndctrls = json_object_new_array();

    for (elem = snd_hctl_first_elem(handle); elem != NULL; elem = snd_hctl_elem_next(elem)) {

        if ((err = snd_hctl_elem_info(elem, info)) < 0) {
            json_object_put(sndctrls); // we abandon request let's free response
            afb_req_fail_f (request, "alsaGetControl devid=[%s/%s] snd_hctl_elem_info error: %s\n"
                           , queryValues.devid, snd_hctl_name(handle), snd_strerror(err));
            goto ExitOnError;           
        }

        // each control is added into a JSON array
        control = alsaGetSingleCtl(elem, info, &queryValues);
        if (control) json_object_array_add(sndctrls, control);

    }

    afb_req_success (request, sndctrls, NULL);
    return;
    
    // nothing special only for debugger breakpoint
    ExitOnError:
        return;
}

// This routine is called when ALSA event are fired
STATIC  int sndCtlEventCB (sd_event_source* src, int fd, uint32_t revents, void* userData) {
    int err;
    evtHandleT *evtHandle = (evtHandleT*)userData; 
    snd_ctl_event_t *ctlEvent;
    json_object *ctlEventJson;
    unsigned int mask;
    int numid;
    int iface;
    int device;
    int subdev;
    const char*devname;
    int index;  
    
    if ((revents & EPOLLHUP) != 0) {
        NOTICE (afbIface, "SndCtl hanghup [car disconnected]");
        goto ExitOnSucess;
    }
    
    if ((revents & EPOLLIN) != 0)  {
          
        snd_ctl_event_alloca(&ctlEvent); // initialise event structure on stack
        
        err = snd_ctl_read(evtHandle->ctl, ctlEvent);
        if (err < 0) goto ExitOnError;
        
        // we only process sndctrl element
        if (snd_ctl_event_get_type(ctlEvent) != SND_CTL_EVENT_ELEM) goto ExitOnSucess;
       
        // we only process value changed events
        mask = snd_ctl_event_elem_get_mask(ctlEvent);
        if (!(mask & SND_CTL_EVENT_MASK_VALUE)) goto ExitOnSucess;
       
        numid  = snd_ctl_event_elem_get_numid(ctlEvent);
        iface  = snd_ctl_event_elem_get_interface(ctlEvent);
        device = snd_ctl_event_elem_get_device(ctlEvent);
        subdev = snd_ctl_event_elem_get_subdevice(ctlEvent);
        devname= snd_ctl_event_elem_get_name(ctlEvent);
        index  = snd_ctl_event_elem_get_index(ctlEvent);
        
        DEBUG(afbIface, "sndCtlEventCB: (%i,%i,%i,%i,%s,%i)", numid, iface, device, subdev, devname, index);

        // proxy ctlevent as a binder event        
        ctlEventJson = json_object_new_object();
        json_object_object_add(ctlEventJson, "numid"  ,json_object_new_int (numid));
        json_object_object_add(ctlEventJson, "iface"  ,json_object_new_int (iface));
        json_object_object_add(ctlEventJson, "device" ,json_object_new_int (device));
        json_object_object_add(ctlEventJson, "subdev" ,json_object_new_int (subdev));
        json_object_object_add(ctlEventJson, "devname",json_object_new_string (devname));
        json_object_object_add(ctlEventJson, "index"  ,json_object_new_int (index));
        afb_event_push(evtHandle->afbevt, ctlEventJson);
    }

    ExitOnSucess:
        return 0;
    
    ExitOnError:
        WARNING (afbIface, "sndCtlEventCB: ignored unsupported event type");
	return (0);    
}

// Loop on every potential Sound card and register active one
PUBLIC void alsaSubCtl (struct afb_req request) {
    static sndHandleT sndHandles[MAX_SND_CARD];
    evtHandleT *evtHandle;
    snd_ctl_t  *ctlHandle;
    snd_ctl_card_info_t *card_info; 
    int err, idx, cardid;
    int idxFree=-1;
    
    const char *devid = afb_req_value(request, "devid");
    if (devid == NULL) {
        afb_req_fail_f (request, "devid-missing", "devid=hw:xxx missing");
        goto ExitOnError;
    }
    
    // open control interface for devid
    err = snd_ctl_open(&ctlHandle, devid, SND_CTL_READONLY);
    if (err < 0) {
    afb_req_fail_f (request, "devid-unknown", "SndCard devid=%s Not Found err=%d", devid, err);
    goto ExitOnError;
    }
    
    // get sound card index use to search existing subscription
    snd_ctl_card_info_alloca(&card_info);  
    snd_ctl_card_info(ctlHandle, card_info);
    cardid = snd_ctl_card_info_get_card(card_info);
    
    // search for an existing subscription and mark 1st free slot
    for (idx= 0; idx < MAX_SND_CARD; idx ++) {
      if (sndHandles[idx].ucount > 0 && sndHandles[idx].cardid == cardid) {
         evtHandle= sndHandles[idx].evtHandle;
         break;
      } else if (idxFree == -1) idxFree= idx; 
    };
      
    // if not subscription exist for the event let's create one
    if (idx == MAX_SND_CARD) {
        
        // reach MAX_SND_CARD event registration
        if (idxFree == -1) {
            afb_req_fail_f (request, "register-toomany", "Cannot register new event Maxcard==%devent name=%s", idx);
            snd_ctl_close(ctlHandle);
            goto ExitOnError;            
        } 
        
        evtHandle = malloc (sizeof(evtHandleT));
        evtHandle->ctl = ctlHandle;
        sndHandles[idxFree].ucount = 0;
        sndHandles[idxFree].cardid = cardid;
        
        // subscribe for sndctl events attached to devid
        err = snd_ctl_subscribe_events(evtHandle->ctl, 1);
        if (err < 0) {
            afb_req_fail_f (request, "subscribe-fail", "Cannot subscribe events from devid=%s err=%d", devid, err);
            snd_ctl_close(ctlHandle);
            goto ExitOnError;
        }
            
        // get pollfd attach to this sound board
        snd_ctl_poll_descriptors(evtHandle->ctl, &evtHandle->pfds, 1);

        // register sound event to binder main loop
        err = sd_event_add_io(afb_daemon_get_event_loop(afbIface->daemon), &evtHandle->src, evtHandle->pfds.fd, EPOLLIN, sndCtlEventCB, evtHandle);
        if (err < 0) {
            afb_req_fail_f (request, "register-mainloop", "Cannot hook events to mainloop devid=%s err=%d", devid, err);
            snd_ctl_close(ctlHandle);
            goto ExitOnError;
        }

        // create binder event attached to devid name
        evtHandle->afbevt = afb_daemon_make_event (afbIface->daemon, devid);
        if (!afb_event_is_valid (evtHandle->afbevt)) {
            afb_req_fail_f (request, "register-event", "Cannot register new binder event name=%s", devid);
            snd_ctl_close(ctlHandle);
            goto ExitOnError; 
        }

        // everything looks OK let's move forward 
        idx=idxFree;
    }
    
    // subscribe to binder event    
    err = afb_req_subscribe(request, evtHandle->afbevt);
    if (err != 0) {
        afb_req_fail_f (request, "register-eventname", "Cannot subscribe binder event name=%s [invalid channel]", devid, err);
        goto ExitOnError;
    }

    json_object *ctlEventJson = json_object_new_object();
    json_object_object_add(ctlEventJson, "test",json_object_new_string ("done"));
    afb_event_push(evtHandle->afbevt, ctlEventJson );
    
    // increase usage count and return success
    sndHandles[idx].ucount ++;
    afb_req_success(request, NULL, NULL);
    return;
    
  ExitOnError:
        return;
}
