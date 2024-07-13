#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/dstr.h>
#include "version.h"
#include "obs-websocket-api.h"

#define OUTPUT_MODE_NONE 0
#define OUTPUT_MODE_ALWAYS 1
#define OUTPUT_MODE_STREAMING 2
#define OUTPUT_MODE_RECORDING 3
#define OUTPUT_MODE_STREAMING_OR_RECORDING 4
#define OUTPUT_MODE_VIRTUAL_CAMERA 5

struct source_record_filter_context {
	obs_source_t* source;
	video_t* video_output;
	audio_t* audio_output;
	bool output_active;
	uint32_t width;
	uint32_t height;
	uint64_t last_frame_time_ns;
	obs_view_t* view;
	bool starting_file_output;
	bool starting_stream_output;
	bool starting_replay_output;
	bool restart;
	obs_output_t* fileOutput;
	obs_output_t* streamOutput;
	obs_output_t* replayOutput;
	obs_encoder_t* encoder;
	obs_encoder_t* aacTrack;
	obs_encoder_t* aacTrack1;
	obs_encoder_t* aacTrack2;
	obs_encoder_t* aacTrack3;
	obs_encoder_t* aacTrack4;
	obs_encoder_t* aacTrack5;
	obs_service_t* service;
	bool record;
	bool stream;
	bool replayBuffer;
	obs_hotkey_pair_id enableHotkey;
	int audio_track;
	bool track_1;
	bool track_2;
	bool track_3;
	bool track_4;
	bool track_5;
	bool track_6;
	obs_weak_source_t* audio_source;
	bool closing;
	long long replay_buffer_duration;
	struct vec4 backgroundColor;
	bool remove_after_record;
	long long record_max_seconds;
	int last_frontend_event;
};

DARRAY(obs_source_t*) source_record_filters;

static void run_queued(obs_task_t task, void* param)
{
	if (obs_in_task_thread(OBS_TASK_UI)) {
		obs_queue_task(OBS_TASK_GRAPHICS, task, param, false);
	}
	else {
		obs_queue_task(OBS_TASK_UI, task, param, false);
	}
}

static const char* source_record_filter_get_name(void* unused)
{
	UNUSED_PARAMETER(unused);
	return "Source Record";
}

struct video_frame {
	uint8_t* data[MAX_AV_PLANES];
	uint32_t linesize[MAX_AV_PLANES];
};

static bool EncoderAvailable(const char* encoder)
{
	const char* val;
	int i = 0;

	while (obs_enum_encoder_types(i++, &val))
		if (strcmp(val, encoder) == 0)
			return true;

	return false;
}

static void calc_min_ts(obs_source_t* parent, obs_source_t* child, void* param)
{
	UNUSED_PARAMETER(parent);
	uint64_t* min_ts = param;
	if (!child || obs_source_audio_pending(child))
		return;
	const uint64_t ts = obs_source_get_audio_timestamp(child);
	if (!ts)
		return;
	if (!*min_ts || ts < *min_ts)
		*min_ts = ts;
}

static void mix_audio(obs_source_t* parent, obs_source_t* child, void* param)
{
	UNUSED_PARAMETER(parent);
	if (!child || obs_source_audio_pending(child))
		return;
	const uint64_t ts = obs_source_get_audio_timestamp(child);
	if (!ts)
		return;
	struct obs_source_audio* mixed_audio = param;
	const size_t pos = (size_t)ns_to_audio_frames(mixed_audio->samples_per_sec, ts - mixed_audio->timestamp);

	if (pos > AUDIO_OUTPUT_FRAMES)
		return;

	const size_t count = AUDIO_OUTPUT_FRAMES - pos;

	struct obs_source_audio_mix child_audio;
	obs_source_get_audio_mix(child, &child_audio);
	for (size_t ch = 0; ch < (size_t)mixed_audio->speakers; ch++) {
		float* out = ((float*)mixed_audio->data[ch]) + pos;
		float* in = child_audio.output[0].data[ch];
		if (!in)
			continue;
		for (size_t i = 0; i < count; i++) {
			out[i] += in[i];
		}
	}
}

static bool audio_input_callback(void* param, uint64_t start_ts_in, uint64_t end_ts_in, uint64_t* out_ts, uint32_t mixers,
	struct audio_output_data* mixes)
{
	UNUSED_PARAMETER(end_ts_in);
	struct source_record_filter_context* filter = param;
	if (filter->closing || obs_source_removed(filter->source)) {
		*out_ts = start_ts_in;
		return true;
	}

	obs_source_t* audio_source = NULL;
	if (filter->audio_source) {
		audio_source = obs_weak_source_get_source(filter->audio_source);
		if (audio_source)
			obs_source_release(audio_source);
	}
	else {
		audio_source = obs_filter_get_parent(filter->source);
	}
	if (!audio_source || obs_source_removed(audio_source)) {
		*out_ts = start_ts_in;
		return true;
	}

	const uint32_t flags = obs_source_get_output_flags(audio_source);
	if ((flags & OBS_SOURCE_COMPOSITE) != 0) {
		uint64_t min_ts = 0;
		obs_source_enum_active_tree(audio_source, calc_min_ts, &min_ts);
		if (min_ts) {
			struct obs_source_audio mixed_audio = { 0 };
			for (size_t i = 0; i < MAX_AUDIO_CHANNELS; i++) {
				mixed_audio.data[i] = (uint8_t*)mixes->data[i];
			}
			mixed_audio.timestamp = min_ts;
			mixed_audio.speakers = audio_output_get_channels(filter->audio_output);
			mixed_audio.samples_per_sec = audio_output_get_sample_rate(filter->audio_output);
			mixed_audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
			obs_source_enum_active_tree(audio_source, mix_audio, &mixed_audio);

			for (size_t mix_idx = 0; mix_idx < MAX_AUDIO_MIXES; mix_idx++) {
				if ((mixers & (1 << mix_idx)) == 0)
					continue;
				// clamp audio
				for (size_t ch = 0; ch < (size_t)mixed_audio.speakers; ch++) {
					float* mix_data = mixes[mix_idx].data[ch];
					float* mix_end = &mix_data[AUDIO_OUTPUT_FRAMES];

					while (mix_data < mix_end) {
						float val = *mix_data;
						val = (val > 1.0f) ? 1.0f : val;
						val = (val < -1.0f) ? -1.0f : val;
						*(mix_data++) = val;
					}
				}
			}
			*out_ts = min_ts;
		}
		else {
			*out_ts = start_ts_in;
		}
		return true;
	}
	if ((flags & OBS_SOURCE_AUDIO) == 0) {
		*out_ts = start_ts_in;
		return true;
	}

	const uint64_t source_ts = obs_source_get_audio_timestamp(audio_source);
	if (!source_ts) {
		*out_ts = start_ts_in;
		return true;
	}

	if (obs_source_audio_pending(audio_source))
		return false;

	struct obs_source_audio_mix audio;
	obs_source_get_audio_mix(audio_source, &audio);

	const size_t channels = audio_output_get_channels(filter->audio_output);
	for (size_t mix_idx = 0; mix_idx < MAX_AUDIO_MIXES; mix_idx++) {
		if ((mixers & (1 << mix_idx)) == 0)
			continue;
		for (size_t ch = 0; ch < channels; ch++) {
			float* out = mixes[mix_idx].data[ch];
			float* in = audio.output[0].data[ch];
			if (!in)
				continue;
			for (size_t i = 0; i < AUDIO_OUTPUT_FRAMES; i++) {
				out[i] += in[i];
				if (out[i] > 1.0f)
					out[i] = 1.0f;
				if (out[i] < -1.0f)
					out[i] = -1.0f;
			}
		}
	}

	*out_ts = source_ts;

	return true;
}

static void start_file_output_task(void* data)
{
	struct source_record_filter_context* context = data;
	if (obs_output_start(context->fileOutput)) {
		if (!context->output_active) {
			context->output_active = true;
			obs_source_inc_showing(obs_filter_get_parent(context->source));
		}
	}
	context->starting_file_output = false;
}

static void start_stream_output_task(void* data)
{
	struct source_record_filter_context* context = data;
	if (obs_output_start(context->streamOutput)) {
		if (!context->output_active) {
			context->output_active = true;
			obs_source_inc_showing(obs_filter_get_parent(context->source));
		}
	}
	context->starting_stream_output = false;
}

void release_output_stopped(void* data, calldata_t* cd)
{
	UNUSED_PARAMETER(cd);
	run_queued((obs_task_t)obs_output_release, data);
}

static void stop_output_task(void* data)
{
	obs_output_t* output = data;
	signal_handler_t* sh = obs_output_get_signal_handler(output);
	if (sh)
		signal_handler_connect(sh, "stop", release_output_stopped, output);
	obs_output_stop(output);
	if (!sh)
		obs_output_release(output);
}

static void force_stop_output_task(void* data)
{
	obs_output_t* output = data;
	signal_handler_t* sh = obs_output_get_signal_handler(output);
	if (sh)
		signal_handler_connect(sh, "stop", release_output_stopped, output);
	obs_output_force_stop(output);
	if (!sh)
		obs_output_release(output);
}

static void start_replay_task(void* data)
{
	struct source_record_filter_context* context = data;
	if (obs_output_start(context->replayOutput)) {
		if (!context->output_active) {
			context->output_active = true;
			obs_source_inc_showing(obs_filter_get_parent(context->source));
		}
	}
	context->starting_replay_output = false;
}

static void ensure_directory(char* path)
{
#ifdef _WIN32
	char* backslash = strrchr(path, '\\');
	if (backslash)
		*backslash = '/';
#endif

	char* slash = strrchr(path, '/');
	if (slash) {
		*slash = 0;
		os_mkdirs(path);
		*slash = '/';
	}

#ifdef _WIN32
	if (backslash)
		*backslash = '\\';
#endif
}

static void remove_filter(void* data, calldata_t* calldata)
{
	UNUSED_PARAMETER(calldata);
	struct source_record_filter_context* filter = data;
	signal_handler_t* sh = obs_output_get_signal_handler(filter->fileOutput);
	signal_handler_disconnect(sh, "stop", remove_filter, filter);
	obs_source_t* source = obs_filter_get_parent(filter->source);
	if (!source && filter->view) {
		source = obs_view_get_source(filter->view, 0);
		obs_source_release(source);
	}
	obs_source_filter_remove(source, filter->source);
}

static const char* get_encoder_id(obs_data_t* settings)
{
	const char* enc_id = obs_data_get_string(settings, "encoder");
	if (strlen(enc_id) == 0 || strcmp(enc_id, "x264") == 0 || strcmp(enc_id, "x264_lowcpu") == 0) {
		enc_id = "obs_x264";
	}
	else if (strcmp(enc_id, "qsv") == 0) {
		enc_id = "obs_qsv11_v2";
	}
	else if (strcmp(enc_id, "qsv_av1") == 0) {
		enc_id = "obs_qsv11_av1";
	}
	else if (strcmp(enc_id, "amd") == 0) {
		enc_id = "h264_texture_amf";
	}
	else if (strcmp(enc_id, "amd_hevc") == 0) {
		enc_id = "h265_texture_amf";
	}
	else if (strcmp(enc_id, "amd_av1") == 0) {
		enc_id = "av1_texture_amf";
	}
	else if (strcmp(enc_id, "nvenc") == 0) {
		enc_id = EncoderAvailable("jim_nvenc") ? "jim_nvenc" : "ffmpeg_nvenc";
	}
	else if (strcmp(enc_id, "nvenc_hevc") == 0) {
		enc_id = EncoderAvailable("jim_hevc_nvenc") ? "jim_hevc_nvenc" : "ffmpeg_hevc_nvenc";
	}
	else if (strcmp(enc_id, "nvenc_av1") == 0) {
		enc_id = "jim_av1_nvenc";
	}
	else if (strcmp(enc_id, "apple_h264") == 0) {
		enc_id = "com.apple.videotoolbox.videoencoder.ave.avc";
	}
	else if (strcmp(enc_id, "apple_hevc") == 0) {
		enc_id = "com.apple.videotoolbox.videoencoder.ave.hevc";
	}
	return enc_id;
}

static void update_video_encoder(struct source_record_filter_context* filter, obs_data_t* settings)
{
	if (obs_encoder_video(filter->encoder) != filter->video_output) {
		if (obs_encoder_active(filter->encoder)) {
			obs_encoder_release(filter->encoder);
			const char* enc_id = get_encoder_id(settings);
			filter->encoder = obs_video_encoder_create(enc_id, obs_source_get_name(filter->source), settings, NULL);
			obs_encoder_set_scaled_size(filter->encoder, 0, 0);
		}
		obs_encoder_set_video(filter->encoder, filter->video_output);
	}
	if (filter->fileOutput && obs_output_get_video_encoder(filter->fileOutput) != filter->encoder)
		obs_output_set_video_encoder(filter->fileOutput, filter->encoder);
	if (filter->streamOutput && obs_output_get_video_encoder(filter->streamOutput) != filter->encoder)
		obs_output_set_video_encoder(filter->streamOutput, filter->encoder);
	if (filter->replayOutput && obs_output_get_video_encoder(filter->replayOutput) != filter->encoder)
		obs_output_set_video_encoder(filter->replayOutput, filter->encoder);
}

static void start_file_output(struct source_record_filter_context* filter, obs_data_t* settings)
{
	obs_data_t* s = obs_data_create();
	char path[512];
	char* filename = os_generate_formatted_filename(obs_data_get_string(settings, "rec_format"), true,
		obs_data_get_string(settings, "filename_formatting"));
	snprintf(path, 512, "%s/%s", obs_data_get_string(settings, "path"), filename);
	bfree(filename);
	ensure_directory(path);
	obs_data_set_string(s, "path", path);
	if (!filter->fileOutput) {
		filter->fileOutput = obs_output_create("ffmpeg_muxer", obs_source_get_name(filter->source), s, NULL);
		if (filter->remove_after_record) {
			signal_handler_t* sh = obs_output_get_signal_handler(filter->fileOutput);
			signal_handler_connect(sh, "stop", remove_filter, filter);
		}
	}
	else {
		obs_output_update(filter->fileOutput, s);
	}
	obs_data_release(s);
	if (filter->encoder) {
		update_video_encoder(filter, settings);
		obs_output_set_video_encoder(filter->fileOutput, filter->encoder);
	}
	int i = 0;
	if (filter->aacTrack) {
		obs_encoder_set_audio(filter->aacTrack, filter->audio_output);
		obs_output_set_audio_encoder(filter->fileOutput, filter->aacTrack, i);
		i++;
	}
	if (filter->aacTrack1) {
		obs_encoder_set_audio(filter->aacTrack1, filter->audio_output);
		obs_output_set_audio_encoder(filter->fileOutput, filter->aacTrack1, i);
		i++;
	}
	if (filter->aacTrack2) {
		obs_encoder_set_audio(filter->aacTrack2, filter->audio_output);
		obs_output_set_audio_encoder(filter->fileOutput, filter->aacTrack2, i);
		i++;
	}
	if (filter->aacTrack3) {
		obs_encoder_set_audio(filter->aacTrack3, filter->audio_output);
		obs_output_set_audio_encoder(filter->fileOutput, filter->aacTrack3, i);
		i++;
	}
	if (filter->aacTrack4) {
		obs_encoder_set_audio(filter->aacTrack4, filter->audio_output);
		obs_output_set_audio_encoder(filter->fileOutput, filter->aacTrack4, i);
		i++;
	}
	if (filter->aacTrack5) {
		obs_encoder_set_audio(filter->aacTrack5, filter->audio_output);
		obs_output_set_audio_encoder(filter->fileOutput, filter->aacTrack5, i);
		i++;
	}


	filter->starting_file_output = true;

	run_queued(start_file_output_task, filter);
}

#define FTL_PROTOCOL "ftl"
#define RTMP_PROTOCOL "rtmp"

static void start_stream_output(struct source_record_filter_context* filter, obs_data_t* settings)
{
	if (!filter->service) {
		filter->service = obs_service_create("rtmp_custom", obs_source_get_name(filter->source), settings, NULL);
	}
	else {
		obs_service_update(filter->service, settings);
	}
	obs_service_apply_encoder_settings(filter->service, settings, NULL);

#if LIBOBS_API_VER < MAKE_SEMANTIC_VERSION(29, 0, 2)
	const char* type = obs_service_get_output_type(filter->service);
#else
	const char* type = obs_service_get_preferred_output_type(filter->service);
#endif
	if (!type) {
		type = "rtmp_output";
#if LIBOBS_API_VER < MAKE_SEMANTIC_VERSION(29, 0, 2)
		const char* url = obs_service_get_url(filter->service);
#else
		const char* url = obs_service_get_connect_info(filter->service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
#endif
		if (url != NULL && strncmp(url, FTL_PROTOCOL, strlen(FTL_PROTOCOL)) == 0) {
			type = "ftl_output";
		}
		else if (url != NULL && strncmp(url, RTMP_PROTOCOL, strlen(RTMP_PROTOCOL)) != 0) {
			type = "ffmpeg_mpegts_muxer";
		}
	}

	if (!filter->streamOutput) {
		filter->streamOutput = obs_output_create(type, obs_source_get_name(filter->source), settings, NULL);
	}
	else {
		obs_output_update(filter->streamOutput, settings);
	}
	obs_output_set_service(filter->streamOutput, filter->service);

	if (filter->encoder) {
		update_video_encoder(filter, settings);
		obs_output_set_video_encoder(filter->streamOutput, filter->encoder);
	}
	int i = 0;
	if (filter->aacTrack) {
		obs_encoder_set_audio(filter->aacTrack, filter->audio_output);
		obs_output_set_audio_encoder(filter->streamOutput, filter->aacTrack, i);
		i++;
	}
	if (filter->aacTrack1) {
		obs_encoder_set_audio(filter->aacTrack1, filter->audio_output);
		obs_output_set_audio_encoder(filter->streamOutput, filter->aacTrack1, i);
		i++;
	}
	if (filter->aacTrack2) {
		obs_encoder_set_audio(filter->aacTrack2, filter->audio_output);
		obs_output_set_audio_encoder(filter->streamOutput, filter->aacTrack2, i);
		i++;
	}
	if (filter->aacTrack3) {
		obs_encoder_set_audio(filter->aacTrack3, filter->audio_output);
		obs_output_set_audio_encoder(filter->streamOutput, filter->aacTrack3, i);
		i++;
	}
	if (filter->aacTrack4) {
		obs_encoder_set_audio(filter->aacTrack4, filter->audio_output);
		obs_output_set_audio_encoder(filter->streamOutput, filter->aacTrack4, i);
		i++;
	}
	if (filter->aacTrack5) {
		obs_encoder_set_audio(filter->aacTrack5, filter->audio_output);
		obs_output_set_audio_encoder(filter->streamOutput, filter->aacTrack5, i);
		i++;
	}

	filter->starting_stream_output = true;

	run_queued(start_stream_output_task, filter);
}

static void start_replay_output(struct source_record_filter_context* filter, obs_data_t* settings)
{
	obs_data_t* s = obs_data_create();

	obs_data_set_string(s, "directory", obs_data_get_string(settings, "path"));
	obs_data_set_string(s, "format", obs_data_get_string(settings, "filename_formatting"));
	obs_data_set_string(s, "extension", obs_data_get_string(settings, "rec_format"));
	obs_data_set_bool(s, "allow_spaces", true);
	filter->replay_buffer_duration = obs_data_get_int(settings, "replay_duration");
	obs_data_set_int(s, "max_time_sec", filter->replay_buffer_duration);
	obs_data_set_int(s, "max_size_mb", 10000);
	if (!filter->replayOutput) {
		obs_data_t* hotkeys = obs_data_get_obj(settings, "replay_hotkeys");
		struct dstr name;
		obs_source_t* parent = obs_filter_get_parent(filter->source);
		if (parent) {
			dstr_init_copy(&name, obs_source_get_name(parent));
			dstr_cat(&name, " - ");
			dstr_cat(&name, obs_source_get_name(filter->source));
		}
		else {
			dstr_init_copy(&name, obs_source_get_name(filter->source));
		}

		filter->replayOutput = obs_output_create("replay_buffer", name.array, s, hotkeys);
		if (filter->remove_after_record) {
			signal_handler_t* sh = obs_output_get_signal_handler(filter->replayOutput);
			signal_handler_connect(sh, "stop", remove_filter, filter);
		}
		dstr_free(&name);
		obs_data_release(hotkeys);
	}
	else {
		obs_output_update(filter->replayOutput, s);
	}
	obs_data_release(s);
	if (filter->encoder) {
		update_video_encoder(filter, settings);
	}
	int i = 0;
	if (filter->aacTrack) {
		obs_encoder_set_audio(filter->aacTrack, filter->audio_output);


		if (obs_output_get_audio_encoder(filter->replayOutput, i) != filter->aacTrack)
			obs_output_set_audio_encoder(filter->replayOutput, filter->aacTrack, i);
		i++;
	}
	if (filter->aacTrack1) {
		obs_encoder_set_audio(filter->aacTrack1, filter->audio_output);

		if (obs_output_get_audio_encoder(filter->replayOutput, i) != filter->aacTrack1)
			obs_output_set_audio_encoder(filter->replayOutput, filter->aacTrack1, i);
		i++;
	}
	if (filter->aacTrack2) {
		obs_encoder_set_audio(filter->aacTrack2, filter->audio_output);

		if (obs_output_get_audio_encoder(filter->replayOutput, i) != filter->aacTrack2)
			obs_output_set_audio_encoder(filter->replayOutput, filter->aacTrack2, i);
		i++;
	}
	if (filter->aacTrack3) {
		obs_encoder_set_audio(filter->aacTrack3, filter->audio_output);

		if (obs_output_get_audio_encoder(filter->replayOutput, i) != filter->aacTrack3)
			obs_output_set_audio_encoder(filter->replayOutput, filter->aacTrack3, i);
		i++;
	}
	if (filter->aacTrack4) {
		obs_encoder_set_audio(filter->aacTrack4, filter->audio_output);

		if (obs_output_get_audio_encoder(filter->replayOutput, i) != filter->aacTrack4)
			obs_output_set_audio_encoder(filter->replayOutput, filter->aacTrack4, i);
		i++;
	}
	if (filter->aacTrack5) {
		obs_encoder_set_audio(filter->aacTrack5, filter->audio_output);

		if (obs_output_get_audio_encoder(filter->replayOutput, i) != filter->aacTrack5)
			obs_output_set_audio_encoder(filter->replayOutput, filter->aacTrack5, i);
		i++;
	}



	filter->starting_replay_output = true;

	run_queued(start_replay_task, filter);
}

bool allFilterTracksTrue(struct source_record_filter_context* filter) {
	return filter->track_1 ||
		filter->track_2 ||
		filter->track_3 ||
		filter->track_4 ||
		filter->track_5 ||
		filter->track_6;
}
bool allSettingsTracksTrue(obs_data_t* settings) {
	bool track_1 = obs_data_get_bool(settings, "track_1");
	bool track_2 = obs_data_get_bool(settings, "track_2");
	bool track_3 = obs_data_get_bool(settings, "track_3");
	bool track_4 = obs_data_get_bool(settings, "track_4");
	bool track_5 = obs_data_get_bool(settings, "track_5");
	bool track_6 = obs_data_get_bool(settings, "track_6");
	return track_1 || track_2 || track_3 || track_4 || track_5 || track_6;
}

static void source_record_filter_update(void* data, obs_data_t* settings)
{

	struct source_record_filter_context* filter = data;

	filter->remove_after_record = obs_data_get_bool(settings, "remove_after_record");
	filter->record_max_seconds = obs_data_get_int(settings, "record_max_seconds");
	const long long record_mode = obs_data_get_int(settings, "record_mode");
	const long long stream_mode = obs_data_get_int(settings, "stream_mode");
	const bool replay_buffer = obs_data_get_bool(settings, "replay_buffer") && !filter->closing;
	if (!filter->closing && (record_mode != OUTPUT_MODE_NONE || stream_mode != OUTPUT_MODE_NONE || replay_buffer)) {

		const char* enc_id = get_encoder_id(settings);
		if (!filter->encoder || strcmp(obs_encoder_get_id(filter->encoder), enc_id) != 0) {
			obs_encoder_release(filter->encoder);
			filter->encoder = obs_video_encoder_create(enc_id, obs_source_get_name(filter->source), settings, NULL);

			obs_encoder_set_scaled_size(filter->encoder, 0, 0);
			obs_encoder_set_video(filter->encoder, filter->video_output);
			if (filter->fileOutput && obs_output_get_video_encoder(filter->fileOutput) != filter->encoder)
				obs_output_set_video_encoder(filter->fileOutput, filter->encoder);
			if (filter->streamOutput && obs_output_get_video_encoder(filter->streamOutput) != filter->encoder)
				obs_output_set_video_encoder(filter->streamOutput, filter->encoder);
			if (filter->replayOutput && obs_output_get_video_encoder(filter->replayOutput) != filter->encoder)
				obs_output_set_video_encoder(filter->replayOutput, filter->encoder);
		}
		else if (!obs_encoder_active(filter->encoder)) {
			obs_encoder_update(filter->encoder, settings);
		}
		const int audio_track = obs_data_get_bool(settings, "different_audio") ? (int)obs_data_get_int(settings, "audio_track") : 0;
		const bool diff_track = obs_data_get_bool(settings, "different_audio");

		bool old_track = allFilterTracksTrue(filter);
		bool new_track = allSettingsTracksTrue(settings);


		if (!filter->audio_output) {
			if (diff_track) {
				filter->audio_output = obs_get_audio();
			}
			else {
				struct audio_output_info oi = { 0 };
				oi.name = obs_source_get_name(filter->source);
				oi.speakers = SPEAKERS_STEREO;
				oi.samples_per_sec = audio_output_get_sample_rate(obs_get_audio());
				oi.format = AUDIO_FORMAT_FLOAT_PLANAR;
				oi.input_param = filter;
				oi.input_callback = audio_input_callback;
				audio_output_open(&filter->audio_output, &oi);
			}
		}
		//else if (audio_track > 0 && filter->audio_track <= 0) {
		else if (old_track && !new_track) {
			audio_output_close(filter->audio_output);
			filter->audio_output = obs_get_audio();
		}
		//else if (audio_track <= 0 && filter->audio_track > 0) {
		else if (!old_track && new_track) {
			filter->audio_output = NULL;
			struct audio_output_info oi = { 0 };
			oi.name = obs_source_get_name(filter->source);
			oi.speakers = SPEAKERS_STEREO;
			oi.samples_per_sec = audio_output_get_sample_rate(obs_get_audio());
			oi.format = AUDIO_FORMAT_FLOAT_PLANAR;
			oi.input_param = filter;
			oi.input_callback = audio_input_callback;
			audio_output_open(&filter->audio_output, &oi);
		}

		//if (!filter->aacTrack || filter->audio_track != audio_track) {
		if (!filter->aacTrack || !filter->aacTrack5 || !filter->aacTrack2 || !filter->aacTrack3 || !filter->aacTrack4 || !filter->aacTrack1) {
			if (filter->aacTrack) {
				obs_encoder_release(filter->aacTrack);
				filter->aacTrack = NULL;
			}
			if (filter->aacTrack1) {
				obs_encoder_release(filter->aacTrack1);
				filter->aacTrack1 = NULL;
			}
			if (filter->aacTrack2) {
				obs_encoder_release(filter->aacTrack2);
				filter->aacTrack2 = NULL;
			}
			if (filter->aacTrack3) {
				obs_encoder_release(filter->aacTrack3);
				filter->aacTrack3 = NULL;
			}
			if (filter->aacTrack4) {
				obs_encoder_release(filter->aacTrack4);
				filter->aacTrack4 = NULL;
			}
			if (filter->aacTrack5) {
				obs_encoder_release(filter->aacTrack5);
				filter->aacTrack5 = NULL;
			}

			if (filter->track_1) {
				filter->aacTrack = obs_audio_encoder_create("ffmpeg_aac", obs_source_get_name(filter->source), NULL, 0, NULL);
			}
			if (filter->track_2) {
				filter->aacTrack1 = obs_audio_encoder_create("ffmpeg_aac", obs_source_get_name(filter->source), NULL, 1, NULL);
			}
			if (filter->track_3) {
				filter->aacTrack2 = obs_audio_encoder_create("ffmpeg_aac", obs_source_get_name(filter->source), NULL, 2, NULL);
			}
			if (filter->track_4) {
				filter->aacTrack3 = obs_audio_encoder_create("ffmpeg_aac", obs_source_get_name(filter->source), NULL, 3, NULL);
			}
			if (filter->track_5) {
				filter->aacTrack4 = obs_audio_encoder_create("ffmpeg_aac", obs_source_get_name(filter->source), NULL, 4, NULL);
			}
			if (filter->track_6) {
				filter->aacTrack5 = obs_audio_encoder_create("ffmpeg_aac", obs_source_get_name(filter->source), NULL, 5, NULL);
			}

			if (!filter->track_1 && !filter->track_2 && !filter->track_3 && !filter->track_4 && !filter->track_5 && !filter->track_6) {
				filter->aacTrack = obs_audio_encoder_create("ffmpeg_aac", obs_source_get_name(filter->source), NULL, 0, NULL);
				filter->aacTrack1 = NULL;
				filter->aacTrack2 = NULL;
				filter->aacTrack3 = NULL;
				filter->aacTrack4 = NULL;
				filter->aacTrack5 = NULL;
			}
			if (filter->audio_output) {
				if (filter->aacTrack) {
					obs_encoder_set_audio(filter->aacTrack, filter->audio_output);
				}
				if (filter->aacTrack5) {
					obs_encoder_set_audio(filter->aacTrack5, filter->audio_output);
				}
				if (filter->aacTrack1) {
					obs_encoder_set_audio(filter->aacTrack1, filter->audio_output);
				}
				if (filter->aacTrack2) {
					obs_encoder_set_audio(filter->aacTrack2, filter->audio_output);
				}
				if (filter->aacTrack3) {
					obs_encoder_set_audio(filter->aacTrack3, filter->audio_output);
				}
				if (filter->aacTrack4) {
					obs_encoder_set_audio(filter->aacTrack4, filter->audio_output);
				}
			}
			if (filter->fileOutput) {
				int i = 0;
				if (filter->aacTrack) {
					obs_output_set_audio_encoder(filter->fileOutput, filter->aacTrack, i);
					i++;
				}
				if (filter->aacTrack1) {
					obs_output_set_audio_encoder(filter->fileOutput, filter->aacTrack1, i);
					i++;
				}
				if (filter->aacTrack2) {
					obs_output_set_audio_encoder(filter->fileOutput, filter->aacTrack2, i);
					i++;
				}
				if (filter->aacTrack3) {
					obs_output_set_audio_encoder(filter->fileOutput, filter->aacTrack3, i);
					i++;
				}
				if (filter->aacTrack4) {
					obs_output_set_audio_encoder(filter->fileOutput, filter->aacTrack4, i);
					i++;
				}
				if (filter->aacTrack5) {
					obs_output_set_audio_encoder(filter->fileOutput, filter->aacTrack5, i);
					i++;
				}

			}


			if (filter->replayOutput) {
				int i = 0;
				if (filter->aacTrack) {
					obs_output_set_audio_encoder(filter->replayOutput, filter->aacTrack, i);
					i++;
				}
				if (filter->aacTrack1) {
					obs_output_set_audio_encoder(filter->replayOutput, filter->aacTrack1, i);
					i++;
				}
				if (filter->aacTrack2) {
					obs_output_set_audio_encoder(filter->replayOutput, filter->aacTrack2, i);
					i++;
				}
				if (filter->aacTrack3) {
					obs_output_set_audio_encoder(filter->replayOutput, filter->aacTrack3, i);
					i++;
				}
				if (filter->aacTrack4) {
					obs_output_set_audio_encoder(filter->replayOutput, filter->aacTrack4, i);
					i++;
				}
				if (filter->aacTrack5) {
					obs_output_set_audio_encoder(filter->replayOutput, filter->aacTrack5, i);
					i++;
				}
			}

		}
		filter->audio_track = audio_track;
		filter->track_1 = obs_data_get_bool(settings, "track_1");
		filter->track_2 = obs_data_get_bool(settings, "track_2");
		filter->track_3 = obs_data_get_bool(settings, "track_3");
		filter->track_4 = obs_data_get_bool(settings, "track_4");
		filter->track_5 = obs_data_get_bool(settings, "track_5");
		filter->track_6 = obs_data_get_bool(settings, "track_6");

	}
	bool record = false;
	if (filter->closing) {
	}
	else if (record_mode == OUTPUT_MODE_ALWAYS) {
		record = true;
	}
	else if (record_mode == OUTPUT_MODE_RECORDING) {
		record = obs_frontend_recording_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_RECORDING_STOPPING &&
			filter->last_frontend_event != OBS_FRONTEND_EVENT_RECORDING_STOPPED;
	}
	else if (record_mode == OUTPUT_MODE_STREAMING) {
		record = obs_frontend_streaming_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_STREAMING_STOPPING &&
			filter->last_frontend_event != OBS_FRONTEND_EVENT_STREAMING_STOPPED;
	}
	else if (record_mode == OUTPUT_MODE_STREAMING_OR_RECORDING) {
		record = (obs_frontend_streaming_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_STREAMING_STOPPING &&
			filter->last_frontend_event != OBS_FRONTEND_EVENT_STREAMING_STOPPED) ||
			(obs_frontend_recording_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_RECORDING_STOPPING &&
				filter->last_frontend_event != OBS_FRONTEND_EVENT_RECORDING_STOPPED);
	}
	else if (record_mode == OUTPUT_MODE_VIRTUAL_CAMERA) {
		record = obs_frontend_virtualcam_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED;
	}

	if (record != filter->record) {
		if (record) {
			if (obs_source_enabled(filter->source) && filter->video_output)
				start_file_output(filter, settings);
		}
		else {
			if (filter->fileOutput) {
				run_queued(stop_output_task, filter->fileOutput);
				filter->fileOutput = NULL;
			}
		}
		filter->record = record;
	}

	if (replay_buffer != filter->replayBuffer) {
		if (replay_buffer) {
			if (obs_source_enabled(filter->source) && filter->video_output)
				start_replay_output(filter, settings);
		}
		else if (filter->replayOutput) {
			obs_data_t* hotkeys = obs_hotkeys_save_output(filter->replayOutput);
			obs_data_set_obj(settings, "replay_hotkeys", hotkeys);
			obs_data_release(hotkeys);
			run_queued(force_stop_output_task, filter->replayOutput);
			filter->replayOutput = NULL;
		}

		filter->replayBuffer = replay_buffer;
	}
	else if (replay_buffer && filter->replayOutput && obs_source_enabled(filter->source)) {
		if (filter->replay_buffer_duration != obs_data_get_int(settings, "replay_duration")) {
			obs_data_t* hotkeys = obs_hotkeys_save_output(filter->replayOutput);
			obs_data_set_obj(settings, "replay_hotkeys", hotkeys);
			obs_data_release(hotkeys);
			run_queued(force_stop_output_task, filter->replayOutput);
			filter->replayOutput = NULL;
			start_replay_output(filter, settings);
		}
	}

	bool stream = false;
	if (filter->closing) {
	}
	else if (stream_mode == OUTPUT_MODE_ALWAYS) {
		stream = true;
	}
	else if (stream_mode == OUTPUT_MODE_RECORDING) {
		stream = obs_frontend_recording_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_RECORDING_STOPPING &&
			filter->last_frontend_event != OBS_FRONTEND_EVENT_RECORDING_STOPPED;
	}
	else if (stream_mode == OUTPUT_MODE_STREAMING) {
		stream = obs_frontend_streaming_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_STREAMING_STOPPING &&
			filter->last_frontend_event != OBS_FRONTEND_EVENT_STREAMING_STOPPED;
	}
	else if (stream_mode == OUTPUT_MODE_STREAMING_OR_RECORDING) {
		stream = (obs_frontend_streaming_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_STREAMING_STOPPING &&
			filter->last_frontend_event != OBS_FRONTEND_EVENT_STREAMING_STOPPED) ||
			(obs_frontend_recording_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_RECORDING_STOPPING &&
				filter->last_frontend_event != OBS_FRONTEND_EVENT_RECORDING_STOPPED);
	}
	else if (stream_mode == OUTPUT_MODE_VIRTUAL_CAMERA) {
		stream = obs_frontend_virtualcam_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED;
	}

	if (stream != filter->stream) {
		if (stream) {
			if (obs_source_enabled(filter->source) && filter->video_output)
				start_stream_output(filter, settings);
		}
		else {
			if (filter->streamOutput) {
				run_queued(force_stop_output_task, filter->streamOutput);
				filter->streamOutput = NULL;
			}
		}
		filter->stream = stream;
	}

	if (!replay_buffer && !record && !stream) {
		if (filter->encoder && !obs_encoder_active(filter->encoder)) {
			obs_encoder_release(filter->encoder);
			filter->encoder = NULL;
		}
		if (filter->aacTrack && !obs_encoder_active(filter->aacTrack)) {
			obs_encoder_release(filter->aacTrack);
			filter->aacTrack = NULL;
		}
		if (filter->aacTrack1 && !obs_encoder_active(filter->aacTrack1)) {
			obs_encoder_release(filter->aacTrack1);
			filter->aacTrack1 = NULL;
		}
		if (filter->aacTrack2 && !obs_encoder_active(filter->aacTrack2)) {
			obs_encoder_release(filter->aacTrack2);
			filter->aacTrack2 = NULL;
		}
		if (filter->aacTrack3 && !obs_encoder_active(filter->aacTrack3)) {
			obs_encoder_release(filter->aacTrack3);
			filter->aacTrack3 = NULL;
		}
		if (filter->aacTrack4 && !obs_encoder_active(filter->aacTrack4)) {
			obs_encoder_release(filter->aacTrack4);
			filter->aacTrack4 = NULL;
		}
		if (filter->aacTrack5 && !obs_encoder_active(filter->aacTrack5)) {
			obs_encoder_release(filter->aacTrack5);
			filter->aacTrack5 = NULL;
		}

	}

	vec4_from_rgba(&filter->backgroundColor, (uint32_t)obs_data_get_int(settings, "backgroundColor"));

	if (obs_data_get_bool(settings, "different_audio")) {
		const char* source_name = obs_data_get_string(settings, "audio_source");
		if (!strlen(source_name)) {
			if (filter->audio_source) {
				obs_weak_source_release(filter->audio_source);
				filter->audio_source = NULL;
			}
		}
		else {
			obs_source_t* source = obs_weak_source_get_source(filter->audio_source);
			if (source)
				obs_source_release(source);
			if (!source || strcmp(source_name, obs_source_get_name(source)) != 0) {
				if (filter->audio_source) {
					obs_weak_source_release(filter->audio_source);
					filter->audio_source = NULL;
				}
				source = obs_get_source_by_name(source_name);
				if (source) {
					filter->audio_source = obs_source_get_weak_source(source);
					obs_source_release(source);
				}
			}
		}

	}
	else if (filter->audio_source) {
		obs_weak_source_release(filter->audio_source);
		filter->audio_source = NULL;
	}
}

static void source_record_filter_save(void* data, obs_data_t* settings)
{
	struct source_record_filter_context* filter = data;
	if (filter->replayOutput) {
		obs_data_t* hotkeys = obs_hotkeys_save_output(filter->replayOutput);
		obs_data_set_obj(settings, "replay_hotkeys", hotkeys);
		obs_data_release(hotkeys);
	}
}

static void source_record_filter_defaults(obs_data_t* settings)
{
	config_t* config = obs_frontend_get_profile_config();

	const char* mode = config_get_string(config, "Output", "Mode");
	const char* type = config_get_string(config, "AdvOut", "RecType");
	const char* adv_path = strcmp(type, "Standard") != 0 || strcmp(type, "standard") != 0
		? config_get_string(config, "AdvOut", "FFFilePath")
		: config_get_string(config, "AdvOut", "RecFilePath");
	bool adv_out = strcmp(mode, "Advanced") == 0 || strcmp(mode, "advanced") == 0;
	const char* rec_path = adv_out ? adv_path : config_get_string(config, "SimpleOutput", "FilePath");

	obs_data_set_default_string(settings, "path", rec_path);
	obs_data_set_default_string(settings, "filename_formatting", config_get_string(config, "Output", "FilenameFormatting"));
	obs_data_set_default_string(settings, "rec_format",
		config_get_string(config, adv_out ? "AdvOut" : "SimpleOutput", "RecFormat"));

	obs_data_set_default_int(settings, "backgroundColor", 0);

	const char* enc_id;
	if (adv_out) {
		enc_id = config_get_string(config, "AdvOut", "RecEncoder");
		if (strcmp(enc_id, "none") == 0 || strcmp(enc_id, "None") == 0)
			enc_id = config_get_string(config, "AdvOut", "Encoder");
		else if (strcmp(enc_id, "jim_nvenc") == 0)
			enc_id = "nvenc";
		else
			obs_data_set_default_string(settings, "encoder", enc_id);
	}
	else {
		const char* quality = config_get_string(config, "SimpleOutput", "RecQuality");
		if (strcmp(quality, "Stream") == 0 || strcmp(quality, "stream") == 0) {
			enc_id = config_get_string(config, "SimpleOutput", "StreamEncoder");
		}
		else if (strcmp(quality, "Lossless") == 0 || strcmp(quality, "lossless") == 0) {
			enc_id = "ffmpeg_output";
		}
		else {
			enc_id = config_get_string(config, "SimpleOutput", "RecEncoder");
		}
		obs_data_set_default_string(settings, "encoder", enc_id);
	}
	obs_data_set_default_int(settings, "replay_duration", 5);
}

static void source_record_filter_filter_remove(void* data, obs_source_t* parent);

static void update_task(void* param)
{
	struct source_record_filter_context* context = param;
	obs_source_update(context->source, NULL);
}

static void frontend_event(enum obs_frontend_event event, void* data)
{
	struct source_record_filter_context* context = data;
	if (event == OBS_FRONTEND_EVENT_STREAMING_STARTING || event == OBS_FRONTEND_EVENT_STREAMING_STARTED ||
		event == OBS_FRONTEND_EVENT_STREAMING_STOPPING || event == OBS_FRONTEND_EVENT_STREAMING_STOPPED ||
		event == OBS_FRONTEND_EVENT_RECORDING_STARTING || event == OBS_FRONTEND_EVENT_RECORDING_STARTED ||
		event == OBS_FRONTEND_EVENT_RECORDING_STOPPING || event == OBS_FRONTEND_EVENT_RECORDING_STOPPED ||
		event == OBS_FRONTEND_EVENT_VIRTUALCAM_STARTED || event == OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED) {
		context->last_frontend_event = (int)event;

		obs_queue_task(OBS_TASK_GRAPHICS, update_task, data, false);
	}
	else if (event == OBS_FRONTEND_EVENT_EXIT || event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP) {
		context->closing = true;
	}
}

static void* source_record_filter_create(obs_data_t* settings, obs_source_t* source)
{
	struct source_record_filter_context* context = bzalloc(sizeof(struct source_record_filter_context));
	context->source = source;

	da_push_back(source_record_filters, &source);
	context->last_frontend_event = -1;
	context->enableHotkey = OBS_INVALID_HOTKEY_PAIR_ID;

	//设置录像模式为 虚拟摄像机
	obs_data_set_default_int(settings, "record_mode", 5);
	obs_data_set_default_string(settings, "rec_format", "mp4");

	source_record_filter_update(context, settings);
	obs_frontend_add_event_callback(frontend_event, context);
	return context;
}

static void source_record_delayed_destroy(void* data)
{
	struct source_record_filter_context* context = data;
	if (context->encoder && obs_encoder_active(context->encoder)) {
		run_queued(source_record_delayed_destroy, context);
		return;
	}
	if (context->aacTrack && obs_encoder_active(context->aacTrack)) {
		run_queued(source_record_delayed_destroy, context);
		return;
	}
	if (context->aacTrack5 && obs_encoder_active(context->aacTrack5)) {
		run_queued(source_record_delayed_destroy, context);
		return;
	}
	if (context->aacTrack1 && obs_encoder_active(context->aacTrack1)) {
		run_queued(source_record_delayed_destroy, context);
		return;
	}
	if (context->aacTrack2 && obs_encoder_active(context->aacTrack2)) {
		run_queued(source_record_delayed_destroy, context);
		return;
	}
	if (context->aacTrack3 && obs_encoder_active(context->aacTrack3)) {
		//run_queued(source_record_delayed_destroy, context);
		return;
	}
	if (context->aacTrack4 && obs_encoder_active(context->aacTrack4)) {
		run_queued(source_record_delayed_destroy, context);
		return;
	}


	if (context->aacTrack != NULL) {
		obs_encoder_release(context->aacTrack);
	}
	if (context->aacTrack5 != NULL) {
		obs_encoder_release(context->aacTrack5);
	}
	if (context->aacTrack1 != NULL) {
		obs_encoder_release(context->aacTrack1);
	}
	if (context->aacTrack2 != NULL) {
		obs_encoder_release(context->aacTrack2);
	}
	if (context->aacTrack3 != NULL) {
		obs_encoder_release(context->aacTrack3);
	}
	if (context->aacTrack4 != NULL) {
		obs_encoder_release(context->aacTrack4);
	}

	//obs_encoder_release(context->aacTrack);
	obs_encoder_release(context->encoder);

	obs_weak_source_release(context->audio_source);
	context->audio_source = NULL;

	if (context->audio_track <= 0)
		audio_output_close(context->audio_output);

	obs_service_release(context->service);

	if (context->video_output) {
		obs_view_set_source(context->view, 0, NULL);
		obs_view_remove(context->view);
		context->video_output = NULL;
	}

	obs_view_destroy(context->view);

	bfree(context);
}

static void source_record_filter_destroy(void* data)
{
	struct source_record_filter_context* context = data;
	da_erase_item(source_record_filters, &context->source);
	context->closing = true;
	if (context->output_active) {
		obs_source_dec_showing(obs_filter_get_parent(context->source));
		context->output_active = false;
	}
	obs_frontend_remove_event_callback(frontend_event, context);

	if (context->fileOutput) {
		run_queued(force_stop_output_task, context->fileOutput);
		context->fileOutput = NULL;
	}
	if (context->streamOutput) {
		run_queued(force_stop_output_task, context->streamOutput);
		context->streamOutput = NULL;
	}
	if (context->replayOutput) {
		run_queued(force_stop_output_task, context->replayOutput);
		context->replayOutput = NULL;
	}

	if (context->enableHotkey != OBS_INVALID_HOTKEY_PAIR_ID)
		obs_hotkey_pair_unregister(context->enableHotkey);

	source_record_delayed_destroy(context);
}

static bool source_record_enable_hotkey(void* data, obs_hotkey_pair_id id, obs_hotkey_t* hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct source_record_filter_context* context = data;
	if (!pressed)
		return false;

	if (obs_source_enabled(context->source))
		return false;

	obs_source_set_enabled(context->source, true);
	return true;
}

static bool source_record_disable_hotkey(void* data, obs_hotkey_pair_id id, obs_hotkey_t* hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct source_record_filter_context* context = data;
	if (!pressed)
		return false;
	if (!obs_source_enabled(context->source))
		return false;
	obs_source_set_enabled(context->source, false);
	return true;
}

static void source_record_filter_tick(void* data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct source_record_filter_context* context = data;
	if (context->closing)
		return;

	obs_source_t* parent = obs_filter_get_parent(context->source);
	if (!parent)
		return;

	if (context->enableHotkey == OBS_INVALID_HOTKEY_PAIR_ID)
		context->enableHotkey = obs_hotkey_pair_register_source(
			parent, "source_record.enable", obs_module_text("SourceRecordEnable"), "source_record.disable",
			obs_module_text("SourceRecordDisable"), source_record_enable_hotkey, source_record_disable_hotkey, context,
			context);

	uint32_t width = obs_source_get_width(parent);
	width += (width & 1);
	uint32_t height = obs_source_get_height(parent);
	height += (height & 1);
	if (context->width != width || context->height != height || (!context->video_output && width && height)) {
		struct obs_video_info ovi = { 0 };
		obs_get_video_info(&ovi);

		ovi.base_width = width;
		ovi.base_height = height;
		ovi.output_width = width;
		ovi.output_height = height;

		if (!context->view)
			context->view = obs_view_create();

		const bool restart = !!context->video_output;
		if (restart)
			obs_view_remove(context->view);

		obs_view_set_source(context->view, 0, parent);

		context->video_output = obs_view_add2(context->view, &ovi);
		if (context->video_output) {
			context->width = width;
			context->height = height;
			if (restart)
				context->restart = true;
		}
	}

	if (context->restart && context->output_active) {
		if (context->fileOutput) {
			run_queued(stop_output_task, context->fileOutput);
			context->fileOutput = NULL;
		}
		if (context->streamOutput) {
			run_queued(stop_output_task, context->streamOutput);
			context->streamOutput = NULL;
		}
		if (context->replayOutput) {
			run_queued(stop_output_task, context->replayOutput);
			context->replayOutput = NULL;
		}
		context->output_active = false;
		context->restart = false;
		obs_source_dec_showing(obs_filter_get_parent(context->source));
	}
	else if (!context->output_active && obs_source_enabled(context->source) &&
		(context->replayBuffer || context->record || context->stream)) {
		if (context->starting_file_output || context->starting_stream_output || context->starting_replay_output ||
			!context->video_output)
			return;
		obs_data_t* s = obs_source_get_settings(context->source);
		if (context->record)
			start_file_output(context, s);
		if (context->stream)
			start_stream_output(context, s);
		if (context->replayBuffer)
			start_replay_output(context, s);
		obs_data_release(s);
	}
	else if (context->output_active && !obs_source_enabled(context->source)) {
		if (context->fileOutput) {
			run_queued(stop_output_task, context->fileOutput);
			context->fileOutput = NULL;
		}
		if (context->streamOutput) {
			run_queued(stop_output_task, context->streamOutput);
			context->streamOutput = NULL;
		}
		if (context->replayOutput) {
			run_queued(stop_output_task, context->replayOutput);
			context->replayOutput = NULL;
		}
		context->output_active = false;
		obs_source_dec_showing(obs_filter_get_parent(context->source));
	}

	if (context->output_active && context->fileOutput && context->record_max_seconds) {
		int totalFrames = obs_output_get_total_frames(context->fileOutput);
		video_t* video = obs_output_video(context->fileOutput);
		uint64_t frameTimeNs = video_output_get_frame_time(video);
		long long msecs = util_mul_div64(totalFrames, frameTimeNs, 1000000ULL);
		if (msecs >= context->record_max_seconds * 1000) {
			obs_data_t* settings = obs_data_create();
			obs_data_set_int(settings, "record_mode", OUTPUT_MODE_NONE);
			obs_source_update(context->source, settings);
			obs_data_release(settings);
		}
	}
}

static bool encoder_changed(void* data, obs_properties_t* props, obs_property_t* property, obs_data_t* settings)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(property);
	UNUSED_PARAMETER(settings);
	obs_properties_remove_by_name(props, "encoder_group");
	bool visible = obs_property_visible(obs_properties_get(props, "others"));
	obs_properties_remove_by_name(props, "others");
	obs_properties_remove_by_name(props, "plugin_info");
	const char* enc_id = get_encoder_id(settings);
	obs_properties_t* enc_props = obs_get_encoder_properties(enc_id);
	if (enc_props) {
		obs_properties_add_group(props, "encoder_group", obs_encoder_get_display_name(enc_id), OBS_GROUP_NORMAL, enc_props);
	}
	obs_property_t* p = obs_properties_add_text(props, "others", obs_module_text("OtherSourceRecords"), OBS_TEXT_INFO);
	obs_property_set_visible(p, visible);
	/* obs_properties_add_text(
		props, "plugin_info",
		"<a href=\"https://obsproject.com/forum/resources/source-record.1285/\">Source Record</a> (" PROJECT_VERSION
		") by <a href=\"https://www.exeldro.com\">Exeldro</a>",
		OBS_TEXT_INFO);*/
	const char* json_str = obs_data_get_json(settings);
	obs_properties_add_text(props, "plugin_info", json_str, OBS_TEXT_INFO);
	return true;
}

static bool list_add_audio_sources(void* data, obs_source_t* source)
{
	obs_property_t* p = data;
	const uint32_t flags = obs_source_get_output_flags(source);
	if ((flags & OBS_SOURCE_COMPOSITE) != 0) {
		obs_property_list_add_string(p, obs_source_get_name(source), obs_source_get_name(source));
	}
	else if ((flags & OBS_SOURCE_AUDIO) != 0) {
		obs_property_list_add_string(p, obs_source_get_name(source), obs_source_get_name(source));
	}
	return true;
}

static obs_properties_t* source_record_filter_properties(void* data)
{
	obs_properties_t* props = obs_properties_create();

	obs_properties_t* record = obs_properties_create();

	obs_property_t* p = obs_properties_add_list(record, "record_mode", obs_module_text("RecordMode"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(p, obs_module_text("None"), OUTPUT_MODE_NONE);
	obs_property_list_add_int(p, obs_module_text("Always"), OUTPUT_MODE_ALWAYS);
	obs_property_list_add_int(p, obs_module_text("Streaming"), OUTPUT_MODE_STREAMING);
	obs_property_list_add_int(p, obs_module_text("Recording"), OUTPUT_MODE_RECORDING);
	obs_property_list_add_int(p, obs_module_text("StreamingOrRecording"), OUTPUT_MODE_STREAMING_OR_RECORDING);
	obs_property_list_add_int(p, obs_module_text("VirtualCamera"), OUTPUT_MODE_VIRTUAL_CAMERA);

	obs_properties_add_path(record, "path", obs_module_text("Path"), OBS_PATH_DIRECTORY, NULL, NULL);
	obs_properties_add_text(record, "filename_formatting", obs_module_text("FilenameFormatting"), OBS_TEXT_DEFAULT);
	p = obs_properties_add_list(record, "rec_format", obs_module_text("RecFormat"), OBS_COMBO_TYPE_EDITABLE,
		OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "flv", "flv");
	obs_property_list_add_string(p, "mp4", "mp4");
	obs_property_list_add_string(p, "mov", "mov");
	obs_property_list_add_string(p, "mkv", "mkv");
	obs_property_list_add_string(p, "ts", "ts");
	obs_property_list_add_string(p, "m3u8", "m3u8");

	obs_properties_add_int(record, "record_max_seconds", obs_module_text("MaxSeconds"), 0, 100000, 1);

	obs_properties_add_group(props, "record", obs_module_text("Record"), OBS_GROUP_NORMAL, record);

	obs_properties_t* replay = obs_properties_create();

	p = obs_properties_add_int(replay, "replay_duration", obs_module_text("Duration"), 1, 1000, 1);
	obs_property_int_set_suffix(p, "s");

	obs_properties_add_group(props, "replay_buffer", obs_module_text("ReplayBuffer"), OBS_GROUP_CHECKABLE, replay);

	obs_properties_t* stream = obs_properties_create();

	p = obs_properties_add_list(stream, "stream_mode", obs_module_text("StreamMode"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(p, obs_module_text("None"), OUTPUT_MODE_NONE);
	obs_property_list_add_int(p, obs_module_text("Always"), OUTPUT_MODE_ALWAYS);
	obs_property_list_add_int(p, obs_module_text("Streaming"), OUTPUT_MODE_STREAMING);
	obs_property_list_add_int(p, obs_module_text("Recording"), OUTPUT_MODE_RECORDING);
	obs_property_list_add_int(p, obs_module_text("StreamingOrRecording"), OUTPUT_MODE_STREAMING_OR_RECORDING);
	obs_property_list_add_int(p, obs_module_text("VirtualCamera"), OUTPUT_MODE_VIRTUAL_CAMERA);

	obs_properties_add_text(stream, "server", obs_module_text("Server"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(stream, "key", obs_module_text("Key"), OBS_TEXT_PASSWORD);

	obs_properties_add_group(props, "stream", obs_module_text("Stream"), OBS_GROUP_NORMAL, stream);

	obs_properties_t* background = obs_properties_create();

	obs_properties_add_color(background, "backgroundColor", obs_module_text("BackgroundColor"));

	obs_properties_add_group(props, "background", obs_module_text("Background"), OBS_GROUP_NORMAL, background);

	obs_properties_t* audio = obs_properties_create();

	//p = obs_properties_add_list(audio, "audio_track", obs_module_text("AudioTrack"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("None"), 0);
	// 添加复选框属性来选择多个音轨
	/* const char *track = obs_module_text("Track");
	for (int i = 1; i <= MAX_AUDIO_MIXES; i++) {
		char buffer[64];
		snprintf(buffer, 64, "%s %i", track, i);
		obs_property_list_add_int(p, buffer, i);
	}*/
	//obs_property_list_add_int(p, obs_module_text("None"), 0);
	for (int i = 1; i <= MAX_AUDIO_MIXES; i++) {
		char buffer[64];
		snprintf(buffer, 64, "track_%d", i);
		char label[64];
		snprintf(label, 64, "%s %d", obs_module_text("Track"), i);
		obs_properties_add_bool(audio, buffer, label);
	}

	p = obs_properties_add_list(audio, "audio_source", obs_module_text("Source"), OBS_COMBO_TYPE_EDITABLE,
		OBS_COMBO_FORMAT_STRING);
	obs_enum_sources(list_add_audio_sources, p);
	obs_enum_scenes(list_add_audio_sources, p);

	obs_properties_add_group(props, "different_audio", obs_module_text("DifferentAudio"), OBS_GROUP_CHECKABLE, audio);

	p = obs_properties_add_list(props, "encoder", obs_module_text("Encoder"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	obs_property_list_add_string(p, obs_module_text("Software"), "x264");
	if (EncoderAvailable("obs_qsv11"))
		obs_property_list_add_string(p, obs_module_text("QSV.H264"), "qsv");
	if (EncoderAvailable("obs_qsv11_av1"))
		obs_property_list_add_string(p, obs_module_text("QSV.AV1"), "qsv_av1");
	if (EncoderAvailable("ffmpeg_nvenc"))
		obs_property_list_add_string(p, obs_module_text("NVENC.H264"), "nvenc");
	if (EncoderAvailable("jim_av1_nvenc"))
		obs_property_list_add_string(p, obs_module_text("NVENC.AV1"), "nvenc_av1");
	if (EncoderAvailable("h265_texture_amf"))
		obs_property_list_add_string(p, obs_module_text("AMD.HEVC"), "amd_hevc");
	if (EncoderAvailable("ffmpeg_hevc_nvenc"))
		obs_property_list_add_string(p, obs_module_text("NVENC.HEVC"), "nvenc_hevc");
	if (EncoderAvailable("h264_texture_amf"))
		obs_property_list_add_string(p, obs_module_text("AMD.H264"), "amd");
	if (EncoderAvailable("av1_texture_amf"))
		obs_property_list_add_string(p, obs_module_text("AMD.AV1"), "amd_av1");
	if (EncoderAvailable("com.apple.videotoolbox.videoencoder.ave.avc"))
		obs_property_list_add_string(p, obs_module_text("Apple.H264"), "apple_h264");
	if (EncoderAvailable("com.apple.videotoolbox.videoencoder.ave.hevc"))
		obs_property_list_add_string(p, obs_module_text("Apple.HEVC"), "apple_hevc");

	const char* enc_id = NULL;
	size_t i = 0;
	while (obs_enum_encoder_types(i++, &enc_id)) {
		if (obs_get_encoder_type(enc_id) != OBS_ENCODER_VIDEO)
			continue;
		const uint32_t caps = obs_get_encoder_caps(enc_id);
		if ((caps & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL)) != 0)
			continue;
		const char* name = obs_encoder_get_display_name(enc_id);
		obs_property_list_add_string(p, name, enc_id);
	}
	obs_property_set_modified_callback2(p, encoder_changed, data);

	obs_properties_t* group = obs_properties_create();
	obs_properties_add_group(props, "encoder_group", obs_module_text("Encoder"), OBS_GROUP_NORMAL, group);

	p = obs_properties_add_text(props, "others", obs_module_text("OtherSourceRecords"), OBS_TEXT_INFO);
	if (data) {
		struct source_record_filter_context* context = data;
		struct dstr sources_text;
		dstr_init(&sources_text);
		for (size_t i = 0; i < source_record_filters.num; i++) {
			if (source_record_filters.array[i] == context->source)
				continue;
			if (sources_text.len)
				dstr_cat(&sources_text, "\n");
			obs_source_t* parent = obs_filter_get_parent(source_record_filters.array[i]);
			if (parent) {
				dstr_cat(&sources_text, obs_source_get_name(parent));
				dstr_cat(&sources_text, " - ");
			}
			dstr_cat(&sources_text, obs_source_get_name(source_record_filters.array[i]));
		}
		if (sources_text.len > 0) {
			obs_data_t* settings = obs_source_get_settings(context->source);
			obs_data_set_string(settings, "others", sources_text.array);
			obs_data_release(settings);
			obs_property_set_visible(p, true);
		}
		else {
			obs_property_set_visible(p, false);
		}
		dstr_free(&sources_text);
	}
	else {
		obs_property_set_visible(p, false);
	}

	/* obs_properties_add_text(
		props, "plugin_info",
		"<a href=\"https://obsproject.com/forum/resources/source-record.1285/\">Source Record</a> (" PROJECT_VERSION
		") by <a href=\"https://www.exeldro.com\">Exeldro</a>",
		OBS_TEXT_INFO);*/
	obs_properties_add_text(props, "plugin_info", data, OBS_TEXT_INFO);
	return props;
}

static void source_record_filter_render(void* data, gs_effect_t* effect)
{
	UNUSED_PARAMETER(effect);
	struct source_record_filter_context* context = data;
	obs_source_skip_video_filter(context->source);
}

static void source_record_filter_filter_remove(void* data, obs_source_t* parent)
{
	UNUSED_PARAMETER(parent);
	struct source_record_filter_context* context = data;
	context->closing = true;
	if (context->fileOutput) {
		run_queued(force_stop_output_task, context->fileOutput);
		context->fileOutput = NULL;
	}
	if (context->streamOutput) {
		run_queued(force_stop_output_task, context->streamOutput);
		context->streamOutput = NULL;
	}
	if (context->replayOutput) {
		run_queued(force_stop_output_task, context->replayOutput);
		context->replayOutput = NULL;
	}
	obs_frontend_remove_event_callback(frontend_event, context);
}

struct obs_source_info source_record_filter_info = {
	.id = "source_record_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = source_record_filter_get_name,
	.create = source_record_filter_create,
	.destroy = source_record_filter_destroy,
	.update = source_record_filter_update,
	.load = source_record_filter_update,
	.save = source_record_filter_save,
	.get_defaults = source_record_filter_defaults,
	.video_render = source_record_filter_render,
	.video_tick = source_record_filter_tick,
	.get_properties = source_record_filter_properties,
	.filter_remove = source_record_filter_filter_remove,
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("source-record", "en-US")
MODULE_EXPORT const char* obs_module_description(void)
{
	return "Source Record Filter";
}

static void* vendor;

static void find_filter(obs_source_t* parent, obs_source_t* child, void* param)
{
	UNUSED_PARAMETER(parent);
	const char* id = obs_source_get_unversioned_id(child);
	if (strcmp(id, "source_record_filter") != 0)
		return;
	obs_source_t** filter = param;
	*filter = child;
}

static void find_source_by_filter(obs_source_t* parent, obs_source_t* child, void* param)
{
	if (strcmp(obs_source_get_unversioned_id(child), "source_record_filter") != 0)
		return;

	DARRAY(obs_source_t*)* sources = param;
	darray_push_back(sizeof(obs_source_t*), &sources->da, &parent);
}

static bool find_source(void* data, obs_source_t* source)
{
	obs_source_enum_filters(source, find_source_by_filter, data);
	return true;
}

obs_source_t* get_source_record_filter(obs_source_t* source, obs_data_t* request_data, obs_data_t* response_data, bool create)
{
	const char* filter_name = obs_data_get_string(request_data, "filter");
	obs_source_t* filter = NULL;
	config_t* config = obs_frontend_get_profile_config();
	if (strlen(filter_name)) {
		filter = obs_source_get_filter_by_name(source, filter_name);
		if (!filter) {
			if (response_data)
				obs_data_set_string(response_data, "error", "filter not found");
			return NULL;
		}
		if (strcmp(obs_source_get_unversioned_id(filter), "source_record_filter") != 0) {
			if (response_data)
				obs_data_set_string(response_data, "error", "filter is not source record filter");
			obs_source_release(filter);
			return NULL;
		}
		struct source_record_filter_context* context = obs_obj_get_data(filter);
		if (context && context->output_active) {
			context->restart = true;
		}
	}
	else {
		obs_source_enum_filters(source, find_filter, &filter);
		filter = obs_source_get_ref(filter);
		if (!filter) {
			if (!create) {
				if (response_data)
					obs_data_set_string(response_data, "error", "failed to find filter");
				return NULL;
			}

			const char* filename = obs_data_get_string(request_data, "filename");
			if (!strlen(filename)) {
				filename = config_get_string(config, "Output", "FilenameFormatting");
			}
			obs_data_t* settings = obs_data_create();
			obs_data_set_bool(settings, "remove_after_record", true);
			char* filter_name = os_generate_formatted_filename(NULL, true, filename);
			filter = obs_source_get_filter_by_name(source, filter_name);
			if (!filter) {
				filter = obs_source_create("source_record_filter", filter_name, settings, NULL);
			}
			else if (strcmp(obs_source_get_unversioned_id(filter), "source_record_filter") != 0) {
				if (response_data)
					obs_data_set_string(response_data, "error", "filter is not source record filter");
				obs_source_release(filter);
				bfree(filter_name);
				obs_data_release(settings);
				return NULL;
			}
			else {
				struct source_record_filter_context* context = obs_obj_get_data(filter);
				if (context && context->output_active) {
					context->restart = true;
				}
			}
			bfree(filter_name);
			obs_data_release(settings);
			if (!filter) {
				if (response_data)
					obs_data_set_string(response_data, "error", "failed to create filter");
				return NULL;
			}
			obs_source_filter_add(source, filter);
		}
	}
	if (!obs_source_enabled(filter))
		obs_source_set_enabled(filter, true);
	return filter;
}

static bool start_record_source(obs_source_t* source, obs_data_t* request_data, obs_data_t* response_data)
{
	obs_source_t* filter = get_source_record_filter(source, request_data, response_data, true);
	if (!filter)
		return false;
	obs_data_t* settings = obs_source_get_settings(filter);
	const char* filename = obs_data_get_string(request_data, "filename");
	struct source_record_filter_context* context = obs_obj_get_data(filter);
	if (context && context->output_active) {
		if (strlen(filename)) {
			if (strstr(filename, "%") || strcmp(filename, obs_data_get_string(settings, "filename_formatting")) != 0) {
				context->restart = true;
			}
		}
		else if (strstr(obs_data_get_string(settings, "filename_formatting"), "%")) {
			context->restart = true;
		}
	}

	if (strlen(filename))
		obs_data_set_string(settings, "filename_formatting", filename);
	if (obs_data_has_user_value(request_data, "max_seconds"))
		obs_data_set_int(settings, "record_max_seconds", obs_data_get_int(request_data, "max_seconds"));
	obs_data_set_int(settings, "record_mode", OUTPUT_MODE_ALWAYS);

	obs_source_update(filter, settings);
	obs_data_release(settings);

	obs_source_release(filter);
	return true;
}

static bool stop_record_source(obs_source_t* source, obs_data_t* request_data, obs_data_t* response_data)
{
	obs_source_t* filter = get_source_record_filter(source, request_data, response_data, false);
	if (!filter)
		return false;

	obs_data_t* settings = obs_data_create();
	obs_data_set_int(settings, "record_mode", OUTPUT_MODE_NONE);
	obs_source_update(filter, settings);
	obs_data_release(settings);
	obs_source_release(filter);
	return true;
}

static void websocket_start_record(obs_data_t* request_data, obs_data_t* response_data, void* param)
{
	UNUSED_PARAMETER(param);
	const char* source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t* source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		if (obs_data_get_bool(request_data, "stop_existing"))
			stop_record_source(source, request_data, NULL);
		success = start_record_source(source, request_data, response_data);
		obs_source_release(source);
	}
	else {
		DARRAY(obs_source_t*) sources = { 0 };
		obs_enum_sources(find_source, &sources);
		obs_enum_scenes(find_source, &sources);
		if (!sources.num) {
			obs_data_set_string(response_data, "error", "no source found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		for (size_t i = 0; i < sources.num; i++) {
			success = start_record_source(sources.array[i], request_data, response_data) && success;
		}
		da_free(sources);
	}
	obs_data_set_bool(response_data, "success", success);
}

static void websocket_stop_record(obs_data_t* request_data, obs_data_t* response_data, void* param)
{
	UNUSED_PARAMETER(param);
	const char* source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t* source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		success = stop_record_source(source, request_data, response_data);
		obs_source_release(source);
	}
	else {
		DARRAY(obs_source_t*) sources = { 0 };
		obs_enum_sources(find_source, &sources);
		obs_enum_scenes(find_source, &sources);
		if (!sources.num) {
			obs_data_set_string(response_data, "error", "no source found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		for (size_t i = 0; i < sources.num; i++) {
			success = stop_record_source(sources.array[i], request_data, response_data) && success;
		}
		da_free(sources);
	}
	obs_data_set_bool(response_data, "success", success);
}

static bool start_replay_buffer_source(obs_source_t* source, obs_data_t* request_data, obs_data_t* response_data)
{
	obs_source_t* filter = get_source_record_filter(source, request_data, response_data, true);
	if (!filter)
		return false;
	obs_data_t* settings = obs_source_get_settings(filter);
	const char* filename = obs_data_get_string(request_data, "filename");
	struct source_record_filter_context* context = obs_obj_get_data(filter);
	if (context && context->output_active) {
		if (strlen(filename)) {
			if (strstr(filename, "%") || strcmp(filename, obs_data_get_string(settings, "filename_formatting")) != 0) {
				context->restart = true;
			}
		}
		else if (strstr(obs_data_get_string(settings, "filename_formatting"), "%")) {
			context->restart = true;
		}
	}

	if (strlen(filename))
		obs_data_set_string(settings, "filename_formatting", filename);

	obs_data_set_bool(settings, "replay_buffer", true);

	obs_source_update(filter, settings);
	obs_data_release(settings);

	obs_source_release(filter);
	return true;
}

static bool stop_replay_buffer_source(obs_source_t* source, obs_data_t* request_data, obs_data_t* response_data)
{
	obs_source_t* filter = get_source_record_filter(source, request_data, response_data, false);
	if (!filter)
		return false;

	obs_data_t* settings = obs_data_create();
	obs_data_set_bool(settings, "replay_buffer", false);
	obs_source_update(filter, settings);
	obs_data_release(settings);
	obs_source_release(filter);
	return true;
}

static bool save_replay_buffer_source(obs_source_t* source, obs_data_t* request_data, obs_data_t* response_data)
{
	obs_source_t* filter = get_source_record_filter(source, request_data, response_data, false);
	if (!filter)
		return false;
	struct source_record_filter_context* context = obs_obj_get_data(filter);
	if (!context->replayOutput)
		return false;

	proc_handler_t* ph = obs_output_get_proc_handler(context->replayOutput);
	calldata_t cd = { 0 };
	bool success = proc_handler_call(ph, "save", &cd);
	calldata_free(&cd);
	obs_source_release(filter);
	return success;
}

static void websocket_start_replay_buffer(obs_data_t* request_data, obs_data_t* response_data, void* param)
{
	UNUSED_PARAMETER(param);
	const char* source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t* source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		if (obs_data_get_bool(request_data, "stop_existing"))
			stop_replay_buffer_source(source, request_data, NULL);
		success = start_replay_buffer_source(source, request_data, response_data);
		obs_source_release(source);
	}
	else {
		DARRAY(obs_source_t*) sources = { 0 };
		obs_enum_sources(find_source, &sources);
		obs_enum_scenes(find_source, &sources);
		if (!sources.num) {
			obs_data_set_string(response_data, "error", "no source found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		for (size_t i = 0; i < sources.num; i++) {
			success = start_replay_buffer_source(sources.array[i], request_data, response_data) && success;
		}
		da_free(sources);
	}
	obs_data_set_bool(response_data, "success", success);
}

static void websocket_stop_replay_buffer(obs_data_t* request_data, obs_data_t* response_data, void* param)
{
	UNUSED_PARAMETER(param);
	const char* source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t* source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		success = stop_replay_buffer_source(source, request_data, response_data);
		obs_source_release(source);
	}
	else {
		DARRAY(obs_source_t*) sources = { 0 };
		obs_enum_sources(find_source, &sources);
		obs_enum_scenes(find_source, &sources);
		if (!sources.num) {
			obs_data_set_string(response_data, "error", "no source found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		for (size_t i = 0; i < sources.num; i++) {
			success = stop_replay_buffer_source(sources.array[i], request_data, response_data) && success;
		}
		da_free(sources);
	}
	obs_data_set_bool(response_data, "success", success);
}

static void websocket_save_replay_buffer(obs_data_t* request_data, obs_data_t* response_data, void* param)
{
	UNUSED_PARAMETER(param);
	const char* source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t* source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		success = save_replay_buffer_source(source, request_data, response_data);
		obs_source_release(source);
	}
	else {
		DARRAY(obs_source_t*) sources = { 0 };
		obs_enum_sources(find_source, &sources);
		obs_enum_scenes(find_source, &sources);
		if (!sources.num) {
			obs_data_set_string(response_data, "error", "no source found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		for (size_t i = 0; i < sources.num; i++) {
			success = save_replay_buffer_source(sources.array[i], request_data, response_data) && success;
		}
		da_free(sources);
	}
	obs_data_set_bool(response_data, "success", success);
}

static bool start_stream_source(obs_source_t* source, obs_data_t* request_data, obs_data_t* response_data)
{
	obs_source_t* filter = get_source_record_filter(source, request_data, response_data, true);
	if (!filter)
		return false;
	obs_data_t* settings = obs_source_get_settings(filter);

	const char* server = obs_data_get_string(request_data, "server");
	if (server && strlen(server))
		obs_data_set_string(settings, "server", server);

	const char* key = obs_data_get_string(request_data, "key");
	if (key && strlen(key))
		obs_data_set_string(settings, "key", key);

	obs_data_set_int(settings, "stream_mode", OUTPUT_MODE_ALWAYS);

	obs_source_update(filter, settings);
	obs_data_release(settings);

	obs_source_release(filter);
	return true;
}

static bool stop_stream_source(obs_source_t* source, obs_data_t* request_data, obs_data_t* response_data)
{
	obs_source_t* filter = get_source_record_filter(source, request_data, response_data, false);
	if (!filter)
		return false;

	obs_data_t* settings = obs_data_create();
	obs_data_set_int(settings, "stream_mode", OUTPUT_MODE_NONE);
	obs_source_update(filter, settings);
	obs_data_release(settings);
	obs_source_release(filter);
	return true;
}

static void websocket_start_stream(obs_data_t* request_data, obs_data_t* response_data, void* param)
{
	UNUSED_PARAMETER(param);
	const char* source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t* source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		if (obs_data_get_bool(request_data, "stop_existing"))
			stop_stream_source(source, request_data, NULL);
		success = start_stream_source(source, request_data, response_data);
		obs_source_release(source);
	}
	else {
		DARRAY(obs_source_t*) sources = { 0 };
		obs_enum_sources(find_source, &sources);
		obs_enum_scenes(find_source, &sources);
		if (!sources.num) {
			obs_data_set_string(response_data, "error", "no source found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		for (size_t i = 0; i < sources.num; i++) {
			success = start_stream_source(sources.array[i], request_data, response_data) && success;
		}
		da_free(sources);
	}
	obs_data_set_bool(response_data, "success", success);
}

static void websocket_stop_stream(obs_data_t* request_data, obs_data_t* response_data, void* param)
{
	UNUSED_PARAMETER(param);
	const char* source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t* source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		success = stop_stream_source(source, request_data, response_data);
		obs_source_release(source);
	}
	else {
		DARRAY(obs_source_t*) sources = { 0 };
		obs_enum_sources(find_source, &sources);
		obs_enum_scenes(find_source, &sources);
		if (!sources.num) {
			obs_data_set_string(response_data, "error", "no source found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		for (size_t i = 0; i < sources.num; i++) {
			success = stop_stream_source(sources.array[i], request_data, response_data) && success;
		}
		da_free(sources);
	}
	obs_data_set_bool(response_data, "success", success);
}

bool obs_module_load(void)
{
	blog(LOG_INFO, "[Source Record] loaded version %s", PROJECT_VERSION);
	obs_register_source(&source_record_filter_info);

	da_init(source_record_filters);

	vendor = obs_websocket_register_vendor("source-record");
	obs_websocket_vendor_register_request(vendor, "record_start", websocket_start_record, NULL);
	obs_websocket_vendor_register_request(vendor, "record_stop", websocket_stop_record, NULL);
	obs_websocket_vendor_register_request(vendor, "replay_buffer_start", websocket_start_replay_buffer, NULL);
	obs_websocket_vendor_register_request(vendor, "replay_buffer_stop", websocket_stop_replay_buffer, NULL);
	obs_websocket_vendor_register_request(vendor, "replay_buffer_save", websocket_save_replay_buffer, NULL);
	obs_websocket_vendor_register_request(vendor, "stream_start", websocket_start_stream, NULL);
	obs_websocket_vendor_register_request(vendor, "stream_stop", websocket_stop_stream, NULL);

	return true;
}

void obs_module_unload(void)
{
	da_free(source_record_filters);
}
