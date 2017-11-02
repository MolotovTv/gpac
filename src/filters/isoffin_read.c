/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2000-2017
 *					All rights reserved
 *
 *  This file is part of GPAC / ISOBMFF reader filter
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include "isoffin.h"

#ifndef GPAC_DISABLE_ISOM

#include <gpac/ismacryp.h>
#include <gpac/media_tools.h>

ISOMChannel *isor_get_channel(ISOMReader *reader, GF_FilterPid *pid)
{
	u32 i=0;
	ISOMChannel *ch;
	while ((ch = (ISOMChannel *)gf_list_enum(reader->channels, &i))) {
		if (ch->pid == pid) return ch;
	}
	return NULL;
}


static GFINLINE Bool isor_is_local(const char *url)
{
	if (!strnicmp(url, "file://", 7)) return GF_TRUE;
	if (!strnicmp(url, "gmem://", 7)) return GF_TRUE;
	if (!strnicmp(url, "isobmff://", 10)) return GF_TRUE;
	if (strstr(url, "://")) return GF_FALSE;
	/*the rest is local (mounted on FS)*/
	return GF_TRUE;
}


static GF_Err isoffin_setup(GF_Filter *filter, ISOMReader *read)
{
	char szURL[2048];
	char *tmp, *src;
	GF_Err e;
	const GF_PropertyValue *prop;
	if (!read) return GF_SERVICE_ERROR;

	if (read->pid) {
		prop = gf_filter_pid_get_property(read->pid, GF_PROP_PID_FILEPATH);
		assert(prop);
		src = prop->value.string;
	} else {
		src = read->src;
	}
	if (!src)  return GF_SERVICE_ERROR;

	read->src_crc = gf_crc_32(src, strlen(src));

	strcpy(szURL, src);
	tmp = strrchr(szURL, '.');
	if (tmp) {
		Bool truncate = GF_TRUE;
		tmp = strchr(tmp, '#');
		if (!tmp && read->pid) {
			prop = gf_filter_pid_get_property(read->pid, GF_PROP_PID_URL);
			if (prop && prop->value.string) {
				tmp = strrchr(prop->value.string, '.');
				if (tmp) tmp = strchr(tmp, '#');
				truncate = GF_FALSE;
			}
		}
		if (tmp) {
			if (!strnicmp(tmp, "#audio", 6)) {
				read->play_only_first_media = GF_ISOM_MEDIA_AUDIO;
			} else if (!strnicmp(tmp, "#video", 6)) {
				read->play_only_first_media = GF_ISOM_MEDIA_VISUAL;
			} else if (!strnicmp(tmp, "#text", 5)) {
				read->play_only_first_media = GF_ISOM_MEDIA_TEXT;
			} else if (!strnicmp(tmp, "#trackID=", 9)) {
				read->play_only_track_id = atoi(tmp+9);
			} else if (!strnicmp(tmp, "#ID=", 4)) {
				read->play_only_track_id = atoi(tmp+4);
			} else {
				read->play_only_track_id = atoi(tmp+1);
			}
			if (truncate) tmp[0] = 0;
		}
	}

	if (! isor_is_local(szURL)) {
		return GF_NOT_SUPPORTED;
	}
	read->start_range = read->end_range = 0;
	prop = gf_filter_pid_get_property(read->pid, GF_PROP_PID_FILE_RANGE);
	if (prop) {
		read->start_range = prop->value.frac.num;
		read->end_range = prop->value.frac.den;
	}

	e = gf_isom_open_progressive(szURL, read->start_range, read->end_range, &read->mov, &read->missing_bytes);

	if (e == GF_ISOM_INCOMPLETE_FILE) {
		read->moov_not_loaded = GF_TRUE;
		return GF_OK;
	}

	if (e != GF_OK) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_NETWORK, ("[IsoMedia] error while opening %s, error=%s\n", szURL,gf_error_to_string(e)));
		gf_filter_setup_failure(filter, e);
		return e;
	}
	read->frag_type = gf_isom_is_fragmented(read->mov) ? 1 : 0;

	read->time_scale = gf_isom_get_timescale(read->mov);

	isor_declare_objects(read);

	return GF_OK;
}

static void isoffin_disconnect(ISOMReader *read)
{
	read->disconnected = GF_TRUE;
	while (gf_list_count(read->channels)) {
		ISOMChannel *ch = (ISOMChannel *)gf_list_get(read->channels, 0);
		gf_list_rem(read->channels, 0);
		isor_reset_reader(ch);
		gf_free(ch);
	}

	if (read->mov) gf_isom_close(read->mov);
	read->mov = NULL;

	read->pid = NULL;
}

static GF_Err isoffin_reconfigure(GF_Filter *filter, ISOMReader *read, const char *next_url)
{
	const GF_PropertyValue *prop;
	u32 i, count;
	Bool is_new_mov = GF_FALSE;
	u64 tfdt;
	u32 trackID, flags=0;
	GF_Err e;

	prop = gf_filter_pid_get_property(read->pid, GF_PROP_PID_FILE_CACHED);
	if (prop && prop->value.boolean) read->input_loaded = GF_TRUE;
	read->refresh_fragmented = GF_FALSE;
	GF_LOG(GF_LOG_DEBUG, GF_LOG_DASH, ("[IsoMedia] reconfigure triggered, URL %s\n", next_url));

	switch (gf_isom_probe_file_range(next_url, read->start_range, read->end_range)) {
	//this is a fragment
	case 3:
		gf_isom_release_segment(read->mov, 1);
		gf_isom_reset_fragment_info(read->mov, 1);

		if (read->no_order_check) flags |= GF_ISOM_SEGMENT_NO_ORDER_FLAG;
#ifdef FILTER_FIXME
		if (scalable_segment) flags |= GF_ISOM_SEGMENT_SCALABLE_FLAG;
#endif
		e = gf_isom_open_segment(read->mov, next_url, read->start_range, read->end_range, flags);
		if (!read->input_loaded && (e==GF_ISOM_INCOMPLETE_FILE)) {
			read->refresh_fragmented = GF_TRUE;
			e = GF_OK;
		}

#ifndef GPAC_DISABLE_LOG
		if (e<0) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_DASH, ("[IsoMedia] Error opening new segment %s at UTC "LLU": %s\n", next_url, gf_net_get_utc(), gf_error_to_string(e) ));
		} else if (read->end_range) {
			GF_LOG(GF_LOG_DEBUG, GF_LOG_DASH, ("[IsoMedia] Playing new range in %s: "LLU"-"LLU"\n", next_url, read->start_range, read->end_range));
		} else {
			GF_LOG(GF_LOG_DEBUG, GF_LOG_DASH, ("[IsoMedia] playing new segment %s (has next dep %d TODO)\n", next_url, 0));
		}
#endif
		break;
	//this is a movie, reload
	case 2:
	case 1:
		tfdt = gf_isom_get_current_tfdt(read->mov, 1);
		GF_LOG(GF_LOG_DEBUG, GF_LOG_DASH, ("[IsoMedia] Switching between files - opening new init segment %s (time offset="LLU") - range "LLU"-"LLU"\n", next_url, tfdt, read->start_range, read->end_range));

		if (gf_isom_is_smooth_streaming_moov(read->mov)) {
			char *tfdt_val = strstr(next_url, "tfdt=");
			//smooth adressing, replace tfdt=0000000000000000000 with proper value
			if (tfdt_val) {
				sprintf(tfdt_val+5, LLU, tfdt);
			} else {
				GF_LOG(GF_LOG_WARNING, GF_LOG_DASH, ("[IsoMedia] Error finding init time for init segment %s at UTC "LLU"\n", next_url, gf_net_get_utc() ));
			}
		}

		if (read->mov) gf_isom_close(read->mov);
		e = gf_isom_open_progressive(next_url, read->start_range, read->end_range, &read->mov, &read->missing_bytes);
		if (e < 0) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_DASH, ("[IsoMedia] Error opening init segment %s at UTC "LLU": %s\n", next_url, gf_net_get_utc(), gf_error_to_string(e) ));
		}
		is_new_mov = GF_TRUE;
		break;
	default:
		return GF_NOT_SUPPORTED;
	}

	gf_filter_post_process_task(filter);

	count = gf_list_count(read->channels);
	
	if (e<0) {
		count = gf_list_count(read->channels);
		gf_isom_release_segment(read->mov, 1);
		//gf_isom_reset_fragment_info(read->mov, 1);
		read->drop_next_segment = 1;
		//error opening the segment, reset everything ...
		gf_isom_reset_fragment_info(read->mov, 0);
		for (i=0; i<count; i++) {
			ISOMChannel *ch = gf_list_get(read->channels, i);
			if (ch)
				ch->sample_num = 0;
		}
		return e;
	}
	//segment is the first in our cache, we may need a refresh
	if (!read->input_loaded) {
		GF_LOG(GF_LOG_DEBUG, GF_LOG_DASH, ("[IsoMedia] Opening current segment in progressive mode (download in progress)\n"));
	} else {
		GF_LOG(GF_LOG_DEBUG, GF_LOG_DASH, ("[IsoMedia] Opening current segment in non-progressive mode (completely downloaded)\n"));
	}

	isor_check_producer_ref_time(read);

	for (i=0; i<count; i++) {
		ISOMChannel *ch = gf_list_get(read->channels, i);
		ch->last_state = GF_OK;
		if (ch->play_state) ch->play_state = 1;
		
		if (ch->base_track) {
#ifdef FILTER_FIXME
			if (scalable_segment)
#endif
			if ((0)) {
				trackID = gf_isom_get_highest_track_in_scalable_segment(read->mov, ch->base_track);
				if (trackID) {
					ch->track_id = trackID;
					ch->track = gf_isom_get_track_by_id(read->mov, ch->track_id);
				}
			} else {
				ch->track = ch->base_track;
				ch->track_id = gf_isom_get_track_id(read->mov, ch->track);
			}
		}

		GF_LOG(GF_LOG_DEBUG, GF_LOG_DASH, ("[IsoMedia] Track %d - cur sample %d - new sample count %d\n", ch->track, ch->sample_num, gf_isom_get_sample_count(ch->owner->mov, ch->track) ));

		//TODO: signal all discontinuities here
		if (is_new_mov) {
			ch->track = gf_isom_get_track_by_id(read->mov, ch->track_id);
			if (!ch->track) {
				if (gf_isom_get_track_count(read->mov)==1) {
					GF_LOG(GF_LOG_DEBUG, GF_LOG_DASH, ("[IsoMedia] Mismatch between track IDs of different representations\n"));
					ch->track = 1;
				} else {
					GF_LOG(GF_LOG_ERROR, GF_LOG_DASH, ("[IsoMedia] Mismatch between track IDs of different representations\n"));
				}
			}

			/*we changed our moov structure, sample_num now starts from 0*/
			ch->sample_num = 0;
		}
#ifdef FILTER_FIXME
		//a loop was detected, our timing is no longer reliable if we use edit lists - just reset the sample time to tfdt ...
		else if (param.url_query.discontinuity_type==2) {
			ch->sample_num = 0;
			if (ch->has_edit_list) {
				ch->sample_time = gf_isom_get_current_tfdt(read->mov, ch->track);
				//next read will query sample for ch->sample_time + 1
				if (ch->sample_time) ch->sample_time--;
			}
		}
#endif
		/*rewrite all upcoming SPS/PPS into the samples*/
		gf_isom_set_nalu_extract_mode(read->mov, ch->track, ch->nalu_extract_mode);
		ch->last_state = GF_OK;

		if (ch->is_cenc) {
			isor_set_crypt_config(ch);
		}
	}
	return e;
}

GF_Err isoffin_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	const GF_PropertyValue *prop;
	ISOMReader *read = gf_filter_get_udta(filter);

	if (is_remove) {
		isoffin_disconnect(read);
		return GF_OK;
	}
	//we must have a file path for now
	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_FILEPATH);
	if (!prop || ! prop->value.string) {
		return GF_NOT_SUPPORTED;
	}

	if (read->pid) {
		const char *next_url = prop->value.string;
		u64 sr, er;
		u32 crc = gf_crc_32(next_url, strlen(next_url) );

		sr = er = 0;
		prop = gf_filter_pid_get_property(read->pid, GF_PROP_PID_FILE_RANGE);
		if (prop) {
			sr = prop->value.frac.num;
			er = prop->value.frac.den;
		}

		if ((read->src_crc == crc) && (read->start_range==sr) && (read->end_range==er))
			return GF_OK;
		read->src_crc = crc;
		read->start_range = sr;
		read->end_range = er;
		
		//we need to reconfigure
		return isoffin_reconfigure(filter, read, next_url);
	}

	read->pid = pid;
	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_FILE_CACHED);
	if (prop && prop->value.boolean) read->input_loaded = GF_TRUE;

	return isoffin_setup(filter, read);
}

GF_Err isoffin_initialize(GF_Filter *filter)
{
	ISOMReader *read = gf_filter_get_udta(filter);

	read->filter = filter;
	read->channels = gf_list_new();

	if (read->src) {
		read->input_loaded = GF_TRUE;
		return isoffin_setup(filter, read);
	}

	return GF_OK;
}


static void isoffin_finalize(GF_Filter *filter)
{
	ISOMReader *read = (ISOMReader *) gf_filter_get_udta(filter);

	read->disconnected = GF_TRUE;

	while (gf_list_count(read->channels)) {
		ISOMChannel *ch = (ISOMChannel *)gf_list_get(read->channels, 0);
		gf_list_rem(read->channels, 0);
		isor_reset_reader(ch);
		gf_free(ch);
	}
	gf_list_del(read->channels);

	if (read->mov) gf_isom_close(read->mov);
	read->mov = NULL;
}


void isor_set_crypt_config(ISOMChannel *ch)
{
	GF_ISOFile *mov = ch->owner->mov;
	u32 track = ch->track;
	u32 scheme_type, scheme_version;
	const char *kms_uri, *scheme_uri;

	if (!ch->is_encrypted) return;

	scheme_type = scheme_version = 0;
	kms_uri = scheme_uri = NULL;

	if (gf_isom_is_ismacryp_media(mov, track, 1)) {
		gf_isom_get_ismacryp_info(mov, track, 1, NULL, &scheme_type, &scheme_version, &scheme_uri, &kms_uri, NULL, NULL, NULL);
	} else if (gf_isom_is_omadrm_media(mov, track, 1)) {
		u32 crypt_type;
		//u8 hash[20];
		gf_isom_get_omadrm_info(mov, track, 1, NULL, &scheme_type, &scheme_version, NULL, &kms_uri, NULL, NULL, NULL, &crypt_type, NULL, NULL, NULL);

		//gf_media_get_file_hash(gf_isom_get_filename(mov), hash);
	} else if (gf_isom_is_cenc_media(mov, track, 1)) {
		ch->is_cenc = GF_TRUE;

		gf_isom_get_cenc_info(ch->owner->mov, ch->track, 1, NULL, &scheme_type, &scheme_version, NULL);
	}
	gf_filter_pid_set_property(ch->pid, GF_PROP_PID_PROTECTION_SCHEME_TYPE, &PROP_UINT(scheme_type) );
	gf_filter_pid_set_property(ch->pid, GF_PROP_PID_PROTECTION_SCHEME_VERSION, &PROP_UINT(scheme_version) );
	if (kms_uri) gf_filter_pid_set_property(ch->pid, GF_PROP_PID_PROTECTION_SCHEME_URI, &PROP_STRING((char*) scheme_uri) );
	if (kms_uri) gf_filter_pid_set_property(ch->pid, GF_PROP_PID_PROTECTION_KMS_URI, &PROP_STRING((char*) kms_uri) );

	if (ch->is_cenc) {
		char *psshd;
		GF_BitStream *pssh_bs = gf_bs_new(NULL, 0, GF_BITSTREAM_WRITE);
		u32 i, s, PSSH_count = gf_isom_get_pssh_count(ch->owner->mov);

		gf_bs_write_u32(pssh_bs, PSSH_count);

		/*fill PSSH in the structure. We will free it in CENC_Setup*/
		for (i=0; i<PSSH_count; i++) {
			GF_CENCPSSHSysInfo info;
			gf_isom_get_pssh_info(ch->owner->mov, i+1, info.SystemID, &info.KID_count, (const bin128 **) & info.KIDs, (const u8 **) &info.private_data, &info.private_data_size);

			gf_bs_write_data(pssh_bs, info.SystemID, 16);
			gf_bs_write_u32(pssh_bs, info.KID_count);
			for (s=0; s<info.KID_count; s++) {
				gf_bs_write_data(pssh_bs, info.KIDs[s], 16);
			}
			gf_bs_write_u32(pssh_bs, info.private_data_size);
			gf_bs_write_data(pssh_bs, info.private_data, info.private_data_size);
		}
		gf_bs_get_content(pssh_bs, &psshd, &s);
		gf_bs_del(pssh_bs);
		gf_filter_pid_set_property(ch->pid, GF_PROP_PID_CENC_PSSH, & PROP_DATA_NO_COPY(psshd, s) );
	}
}


ISOMChannel *isor_create_channel(ISOMReader *read, GF_FilterPid *pid, u32 track, u32 item_id)
{
	ISOMChannel *ch;
	const GF_PropertyValue *p;
	if (!read->mov) return NULL;

	GF_SAFEALLOC(ch, ISOMChannel);
	if (!ch) {
		return NULL;
	}
	ch->owner = read;
	ch->pid = pid;
	ch->to_init = GF_TRUE;
	gf_list_add(read->channels, ch);
	ch->track = track;
	ch->item_id = item_id;

	ch->nalu_extract_mode = 0;
	ch->track_id = gf_isom_get_track_id(read->mov, ch->track);
	switch (gf_isom_get_media_type(ch->owner->mov, ch->track)) {
	case GF_ISOM_MEDIA_OCR:
		ch->streamType = GF_STREAM_OCR;
		break;
	case GF_ISOM_MEDIA_SCENE:
		ch->streamType = GF_STREAM_SCENE;
		break;
	case GF_ISOM_MEDIA_VISUAL:
		gf_isom_get_reference(ch->owner->mov, ch->track, GF_ISOM_REF_BASE, 1, &ch->base_track);
		//use base track only if avc/svc or hevc/lhvc. If avc+lhvc we need different rules
		if ( gf_isom_get_avc_svc_type(ch->owner->mov, ch->base_track, 1) == GF_ISOM_AVCTYPE_AVC_ONLY) {
			if ( gf_isom_get_hevc_lhvc_type(ch->owner->mov, ch->track, 1) >= GF_ISOM_HEVCTYPE_HEVC_ONLY) {
				ch->base_track=0;
			}
		}
		ch->next_track = 0;
		/*in scalable mode add SPS/PPS in-band*/
		ch->nalu_extract_mode = GF_ISOM_NALU_EXTRACT_INBAND_PS_FLAG /*| GF_ISOM_NALU_EXTRACT_ANNEXB_FLAG*/;
		break;
	}

	ch->has_edit_list = gf_isom_get_edit_list_type(ch->owner->mov, ch->track, &ch->dts_offset) ? GF_TRUE : GF_FALSE;
	ch->has_rap = (gf_isom_has_sync_points(ch->owner->mov, ch->track)==1) ? GF_TRUE : GF_FALSE;
	ch->time_scale = gf_isom_get_media_timescale(ch->owner->mov, ch->track);

	if (!track || !gf_isom_is_track_encrypted(read->mov, track)) return ch;

	ch->is_encrypted = GF_TRUE;
	p = gf_filter_pid_get_property(pid, GF_PROP_PID_STREAM_TYPE);
	if (p) gf_filter_pid_set_property(pid, GF_PROP_PID_ORIG_STREAM_TYPE, &PROP_UINT(p->value.uint) );
	gf_filter_pid_set_property(pid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_ENCRYPTED) );

	isor_set_crypt_config(ch);

	if (ch->nalu_extract_mode) {
		if (ch->is_encrypted) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_CONTAINER, ("[IsoMedia] using sample NAL rewrite with encryption is not yet supported, patch welcome\n"));
		} else {
			gf_isom_set_nalu_extract_mode(ch->owner->mov, ch->track, ch->nalu_extract_mode);
		}
	}
	return ch;
}

/*switch channel quality. Return next channel or current channel if error*/
static
u32 isoffin_channel_switch_quality(ISOMChannel *ch, GF_ISOFile *the_file, Bool switch_up)
{
	u32 i, count, next_track, trackID, cur_track;
	s32 ref_count;

	cur_track = ch->next_track ? ch->next_track : ch->track;
	count = gf_isom_get_track_count(the_file);
	trackID = gf_isom_get_track_id(the_file, cur_track);
	next_track = 0;

	if (switch_up) {
		for (i = 0; i < count; i++) {
			ref_count = gf_isom_get_reference_count(the_file, i+1, GF_ISOM_REF_SCAL);
			if (ref_count < 0)
				return cur_track; //error
			if (ref_count == 0)
				continue;
			/*next track is the one that has the last reference of type GF_ISOM_REF_SCAL refers to this current track*/
			if ((u32)ref_count == gf_isom_has_track_reference(the_file, i+1, GF_ISOM_REF_SCAL, trackID)) {
				next_track = i+1;
				break;
			}
		}
		/*this is the highest quality*/
		if (!next_track)
			return cur_track;
	} else {
		if (cur_track == ch->base_track)
			return cur_track;
		ref_count = gf_isom_get_reference_count(the_file, cur_track, GF_ISOM_REF_SCAL);
		if (ref_count <= 0)
			return cur_track;
		gf_isom_get_reference(the_file, cur_track, GF_ISOM_REF_SCAL, ref_count, &next_track);
		if (!next_track)
			return cur_track;
	}

	/*in scalable mode add SPS/PPS in-band*/
	gf_isom_set_nalu_extract_mode(the_file, next_track, ch->nalu_extract_mode);

	return next_track;
}

static Bool isoffin_process_event(GF_Filter *filter, const GF_FilterEvent *com)
{
	u32 count, i;
	ISOMChannel *ch;
	ISOMReader *read = gf_filter_get_udta(filter);

	if (!read || read->disconnected) return GF_FALSE;

#ifdef FILTER_FIXME
	if (com->type==GF_NET_SERVICE_INFO) {
		u32 tag_len;
		const char *tag;
		if (gf_isom_apple_get_tag(read->mov, GF_ISOM_ITUNE_NAME, &tag, &tag_len)==GF_OK) com->info.name = tag;
		if (gf_isom_apple_get_tag(read->mov, GF_ISOM_ITUNE_ARTIST, &tag, &tag_len)==GF_OK) com->info.artist = tag;
		if (gf_isom_apple_get_tag(read->mov, GF_ISOM_ITUNE_ALBUM, &tag, &tag_len)==GF_OK) com->info.album = tag;
		if (gf_isom_apple_get_tag(read->mov, GF_ISOM_ITUNE_COMMENT, &tag, &tag_len)==GF_OK) com->info.comment = tag;
		if (gf_isom_apple_get_tag(read->mov, GF_ISOM_ITUNE_TRACK, &tag, &tag_len)==GF_OK) {
			com->info.track_info = (((tag[2]<<8)|tag[3]) << 16) | ((tag[4]<<8)|tag[5]);
		}
		if (gf_isom_apple_get_tag(read->mov, GF_ISOM_ITUNE_COMPOSER, &tag, &tag_len)==GF_OK) com->info.composer = tag;
		if (gf_isom_apple_get_tag(read->mov, GF_ISOM_ITUNE_WRITER, &tag, &tag_len)==GF_OK) com->info.writer = tag;
		if (gf_isom_apple_get_tag(read->mov, GF_ISOM_ITUNE_GENRE, &tag, &tag_len)==GF_OK) {
			if (tag[0]) {
				com->info.genre = 0;
			} else {
				com->info.genre = (tag[0]<<8) | tag[1];
			}
		}
		return GF_OK;
	}
#endif

	if (com->base.type == GF_FEVT_QUALITY_SWITCH) {
		count = gf_list_count(read->channels);
		for (i = 0; i < count; i++) {
			ch = (ISOMChannel *)gf_list_get(read->channels, i);
			if (ch->base_track && gf_isom_needs_layer_reconstruction(read->mov)) {
				ch->next_track = isoffin_channel_switch_quality(ch, read->mov, com->quality_switch.up);
			}
		}
		return GF_OK;
	}

	if (!com->base.on_pid) return GF_FALSE;

	ch = isor_get_channel(read, com->base.on_pid);
	if (!ch)
		return GF_FALSE;

	switch (com->base.type) {
#ifdef FILTER_FIXME
	case GF_NET_CHAN_NALU_MODE:
		ch->nalu_extract_mode = GF_ISOM_NALU_EXTRACT_INBAND_PS_FLAG;

		//when this is set, we work in real scalable (eg N streams reassembled by the player) so only extract the layer. This wll need refinements if we plan to support
		//several scalable layers ...
		if (com->nalu_mode.extract_mode>=1) {
			if (com->nalu_mode.extract_mode==2) {
				ch->disable_seek = 1;
			}
			ch->nalu_extract_mode |= GF_ISOM_NALU_EXTRACT_ANNEXB_FLAG | GF_ISOM_NALU_EXTRACT_VDRD_FLAG | GF_ISOM_NALU_EXTRACT_LAYER_ONLY;
		}
		gf_isom_set_nalu_extract_mode(ch->owner->mov, ch->track, ch->nalu_extract_mode);
		break;
#endif

	case GF_FEVT_PLAY:
		isor_reset_reader(ch);
		ch->speed = com->play.speed;
		read->reset_frag_state = 1;
		if (read->frag_type)
			read->frag_type = 1;

		ch->start = ch->end = 0;
		if (com->play.speed>0) {
			Double t;
			if (com->play.start_range>=0) {
				t = com->play.start_range;
				t *= ch->time_scale;
				ch->start = (u64) t;
			}
			if (com->play.end_range >= com->play.start_range) {
				t = com->play.end_range;
				t *= ch->time_scale;
				ch->end = (u64) t;
			}
		} else if (com->play.speed<0) {
			Double end = com->play.end_range;
			if (end==-1) end = 0;
			ch->start = (u64) (s64) (com->play.start_range * ch->time_scale);
			if (end <= com->play.start_range)
				ch->end = (u64) (s64) (end  * ch->time_scale);
		}
		ch->play_state = 1;

		GF_LOG(GF_LOG_DEBUG, GF_LOG_NETWORK, ("[IsoMedia] Starting channel playback "LLD" to "LLD" (%g to %g)\n", ch->start, ch->end, com->play.start_range, com->play.end_range));

		if (!read->nb_playing)
			gf_isom_reset_seq_num(read->mov);

		if (read->is_partial_download) read->input_loaded = GF_FALSE;

		if (!read->nb_playing && read->pid && !read->input_loaded) {
			GF_FilterEvent fevt;
			u64 max_offset = GF_FILTER_NO_BO;
			u32 i, count = gf_list_count(read->channels);
			for (i=0; i< count; i++) {
				u32 mode, sample_desc_index, sample_num;
				u64 data_offset;
				GF_Err e;
				u64 time;
				ch = gf_list_get(read->channels, i);
				mode = ch->disable_seek ? GF_ISOM_SEARCH_BACKWARD : GF_ISOM_SEARCH_SYNC_BACKWARD;
				time = com->play.start_range;
				time *= ch->time_scale;

				/*take care of seeking out of the track range*/
				if (!read->frag_type && (ch->duration < time)) {
					e = gf_isom_get_sample_for_movie_time(read->mov, ch->track, ch->duration, 	&sample_desc_index, mode, NULL, &sample_num, &data_offset);
				} else {
					e = gf_isom_get_sample_for_movie_time(read->mov, ch->track, time, &sample_desc_index, mode, NULL, &sample_num, &data_offset);
				}
				if ((e == GF_OK) && (data_offset<max_offset))
					max_offset = data_offset;
			}

			if ((com->play.start_range || read->is_partial_download)  && (max_offset != GF_FILTER_NO_BO) ) {

				//send a seek request
				read->is_partial_download = GF_TRUE;
				read->wait_for_source = GF_TRUE;

				//post a seek from 0 - TODO we could build a map of byte offsets
				GF_FEVT_INIT(fevt, GF_FEVT_SOURCE_SEEK, read->pid);
				fevt.seek.start_offset = max_offset;
				gf_filter_pid_send_event(read->pid, &fevt);
				gf_isom_set_byte_offset(read->mov, max_offset);

			}
		}
		//always request a process task upon a play
		gf_filter_post_process_task(read->filter);

		read->nb_playing++;
		//cancel event
		return GF_TRUE;

	case GF_FEVT_STOP:
		if (read->nb_playing) read->nb_playing--;
		isor_reset_reader(ch);
		//cancel event
		return GF_FALSE;

	case GF_FEVT_SET_SPEED:
		ch->speed = com->play.speed;
		//cancel event
		return GF_TRUE;
	default:
		break;
	}
	//by default don't cancel event - to rework once we have downloading in place
	return GF_FALSE;
}

static GF_Err isoffin_process(GF_Filter *filter)
{
	ISOMReader *read = gf_filter_get_udta(filter);
	u32 i, count = gf_list_count(read->channels);
	Bool is_active = GF_FALSE;
	Bool in_is_eos = GF_FALSE;

	if (read->pid) {
		while (1) {
			GF_FilterPacket *pck = gf_filter_pid_get_packet(read->pid);
			if (!pck) {
				//we issued a seek, wait for the first packet to be received before fetching channels
				//otherwise we could end up reading from the wrong cache
				if (read->wait_for_source) {
					//something went wrong during the seek request
					if (gf_filter_pid_is_eos(read->pid))
						return GF_EOS;
					return GF_OK;
				}
				break;
			}
			read->wait_for_source = GF_FALSE;
			gf_filter_pid_drop_packet(read->pid);
		}
		if (gf_filter_pid_is_eos(read->pid)) {
			in_is_eos = GF_TRUE;
			read->input_loaded = GF_TRUE;
		}
	}
	if (read->moov_not_loaded) {
		read->moov_not_loaded = GF_FALSE;
		return isoffin_setup(filter, read);
	}

	if (read->refresh_fragmented) {
		u64 bytesMissing=0;
		GF_Err e;
		e = gf_isom_refresh_fragmented(read->mov, &bytesMissing, /*TODO for mem storage*/NULL);

		if (e && (e!= GF_ISOM_INCOMPLETE_FILE)) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_DASH, ("[IsoMedia] Failed to refresh current segment: %s\n", gf_error_to_string(e) ));
		} else {
			GF_LOG(GF_LOG_DEBUG, GF_LOG_DASH, ("[IsoMedia] Refreshing current segment at UTC "LLU" - "LLU" bytes still missing\n", gf_net_get_utc(), bytesMissing ));
		}
#ifndef GPAC_DISABLE_LOG
		if (gf_log_tool_level_on(GF_LOG_DASH, GF_LOG_DEBUG)) {
			for (i=0; i<count; i++) {
				ISOMChannel *ch = gf_list_get(read->channels, i);
				GF_LOG(GF_LOG_DEBUG, GF_LOG_DASH, ("[IsoMedia] refresh track %d fragment - cur sample %d - new sample count %d\n", ch->track, ch->sample_num, gf_isom_get_sample_count(ch->owner->mov, ch->track) ));
			}
		}
#endif
		isor_check_producer_ref_time(read);

	}

	for (i=0; i<count; i++) {
		char *data;
		ISOMChannel *ch;
		ch = gf_list_get(read->channels, i);
		if (ch->play_state != 1) continue;
		is_active = GF_TRUE;

		while (! gf_filter_pid_would_block(ch->pid) ) {

			if (ch->item_id) {
				isor_reader_get_sample_from_item(ch);
			} else {
				isor_reader_get_sample(ch);
			}

			if (ch->sample) {
				u32 sample_dur;
				GF_FilterPacket *pck;
				pck = gf_filter_pck_new_alloc(ch->pid, ch->sample->dataLength, &data);
				assert(pck);
				
				memcpy(data, ch->sample->data, ch->sample->dataLength);

				gf_filter_pck_set_dts(pck, ch->current_slh.decodingTimeStamp);
				gf_filter_pck_set_cts(pck, ch->current_slh.compositionTimeStamp);
				gf_filter_pck_set_sap(pck, ch->sample->IsRAP);
				sample_dur = gf_isom_get_sample_duration(read->mov, ch->track, ch->sample_num);
				gf_filter_pck_set_duration(pck, sample_dur);
				gf_filter_pck_set_seek_flag(pck, ch->current_slh.seekFlag);

				gf_filter_pck_set_property(pck, GF_PROP_PCK_ENCRYPTED, &PROP_BOOL(ch->current_slh.cenc_encrypted) );
				if (ch->current_slh.cenc_encrypted) {
					gf_filter_pck_set_property(pck, GF_PROP_PID_PCK_CENC_PATTERN, &PROP_FRAC_INT(ch->current_slh.skip_byte_block, ch->current_slh.crypt_byte_block) );

					if (ch->current_slh.cenc_encrypted && !ch->current_slh.IV_size) {
						gf_filter_pck_set_property(pck, GF_PROP_PID_PCK_CENC_IV_CONST, &PROP_DATA(ch->current_slh.constant_IV, ch->current_slh.constant_IV_size) );
					} else {
						gf_filter_pck_set_property(pck, GF_PROP_PID_PCK_CENC_IV_SIZE, &PROP_UINT(ch->current_slh.IV_size) );
					}
					gf_filter_pck_set_property(pck, GF_PROP_PCK_CENC_SAI, &PROP_DATA(ch->sai_buffer, ch->sai_buffer_size) );
				}
				
				gf_filter_pck_send(pck);
				isor_reader_release_sample(ch);
			} else if (ch->last_state==GF_EOS) {
				assert(read->input_loaded);
				if (in_is_eos && (ch->play_state==1)) {
					ch->play_state = 2;
					gf_filter_pid_set_eos(ch->pid);
				}
				break;
			} else {
				break;
			}
		}
	}
	return is_active ? GF_OK : GF_EOS;
}

#define OFFS(_n)	#_n, offsetof(ISOMReader, _n)

static const GF_FilterArgs ISOFFInArgs[] =
{
	{ OFFS(src), "location of source content", GF_PROP_NAME, NULL, NULL, GF_FALSE},
	{}
};

static const GF_FilterCapability ISOFFInInputs[] =
{
	CAP_INC_STRING(GF_PROP_PID_MIME, "application/x-isomedia|application/mp4|video/mp4|audio/mp4|video/3gpp|audio/3gpp|video/3gp2|audio/3gp2|video/iso.segment|audio/iso.segment|image/heif|image/heic|image/avci"),
	{},
	CAP_INC_STRING(GF_PROP_PID_FILE_EXT, "mp4|mpg4|m4a|m4i|3gp|3gpp|3g2|3gp2|iso|m4s|heif|heic|avci"),
};

static const GF_FilterCapability ISOFFInOutputs[] =
{
	CAP_INC_UINT(GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
	CAP_INC_UINT(GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
//	CAP_INC_UINT(GF_PROP_PID_STREAM_TYPE, GF_STREAM_SCENE),
//	CAP_INC_UINT(GF_PROP_PID_STREAM_TYPE, GF_STREAM_OD),
};

GF_FilterRegister ISOFFInRegister = {
	.name = "mp4in",
	.description = "ISOFF Demuxer",
	.private_size = sizeof(ISOMReader),
	.args = ISOFFInArgs,
	.initialize = isoffin_initialize,
	.finalize = isoffin_finalize,
	.process = isoffin_process,
	.configure_pid = isoffin_configure_pid,
	INCAPS(ISOFFInInputs),
	OUTCAPS(ISOFFInOutputs),
	.process_event = isoffin_process_event
};


#endif /*GPAC_DISABLE_ISOM*/

const GF_FilterRegister *isoffin_register(GF_FilterSession *session)
{
#ifdef GPAC_DISABLE_ISOM
	return NULL;
#else
	return &ISOFFInRegister;
#endif /*GPAC_DISABLE_ISOM*/
}

