#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <glib.h>
#include <gmime/gmime.h>
#include <gio/gunixinputstream.h>
#include <json-glib/json-glib.h>

static const char* DEFAULT_FILENAME = "-";
static const char* DEFAULT_CONTENT_TYPE = "application/octet-stream";

typedef struct {
	const char* filename;
	gboolean wantOcr;
	gboolean wantSplitByPage;
	const char* languageCode;
	JsonObject* metadata;

	JsonParser* parser;
} InputJson;

typedef struct {
	const char* mime_boundary;
	GMimeStream* stream;
} Out;

void
dup_json_object_child(JsonObject* src, const char* member_name, JsonNode* member_node, gpointer user_data)
{
	JsonObject* dest = (JsonObject*) user_data;
	JsonNode* member_node_copy = json_node_copy(member_node);
	json_object_set_member(dest, member_name, member_node_copy);
}

void
begin_mime_part(Out* out, const char* name)
{
	g_mime_stream_printf(
		out->stream,
		"\r\n--%s\r\nContent-Disposition: form-data; name=%s\r\n\r\n",
		out->mime_boundary,
		name
	);
}

void
begin_child_mime_part(Out* out, int index, const char* suffix)
{
	g_mime_stream_printf(
		out->stream,
		"\r\n--%s\r\nContent-Disposition: form-data; name=%d%s\r\n\r\n",
		out->mime_boundary,
		index,
		suffix
	);
}

void
end_mime_message(Out* out)
{
	g_mime_stream_printf(out->stream, "\r\n--%s--", out->mime_boundary);
}

void
fail(Out* out, const char* error)
{
	fprintf(stderr, "%s\n", error);
	begin_mime_part(out, "error");
	g_mime_stream_write_string(out->stream, error);
	end_mime_message(out);
	g_object_unref(out->stream);
	exit(0);
}

void
write_output_json(
	Out* out,
	int child_index,
	const char* filename,
	const char* content_type,
	JsonObject* output_json
)
{
	begin_child_mime_part(out, child_index, ".json");

	JsonObject* json = json_object_new();
	json_object_foreach_member(output_json, dup_json_object_child, json);
	json_object_set_string_member(json, "contentType", content_type);

	if (filename != NULL && filename[0] != '\0') {
		/* Set "filename" to "foo.eml/attachment.doc" */
		GString* full_filename_s = g_string_new(json_object_get_string_member(json, "filename"));
		g_string_append_c(full_filename_s, '/');
		g_string_append(full_filename_s, filename);
		char* full_filename = g_string_free(full_filename_s, FALSE);
		json_object_set_string_member(json, "filename", full_filename);
		g_free(full_filename);
	}

	JsonNode* root = json_node_alloc();
	json_node_init_object(root, json);
	json_object_unref(json);

	JsonGenerator* generator = json_generator_new();
	json_generator_set_root(generator, root);
	json_node_unref(root);

	char* json_bytes = json_generator_to_data(generator, NULL);
	g_mime_stream_write_string(out->stream, json_bytes);
	g_free(json_bytes);

	g_object_unref(generator);
}

InputJson
load_input_json(Out* out, const char* input_json_string)
{
	InputJson ret;

	ret.parser = json_parser_new_immutable();
	json_parser_load_from_data(ret.parser, input_json_string, -1, NULL);

	JsonNode* root_node = json_parser_get_root(ret.parser);
	JsonObject* root = json_node_get_object(root_node);
	if (!root) fail(out, "Input JSON was not an Object");

	ret.filename = json_object_get_string_member(root, "filename");
	if (!ret.filename) fail(out, "Input JSON is missing string 'filename'");

	ret.languageCode = json_object_get_string_member(root, "languageCode");
	if (!ret.languageCode) fail(out, "Input JSON is missing string 'languageCode'");

	/* booleans can't be null */
	ret.wantOcr = json_object_get_boolean_member(root, "wantOcr");
	ret.wantSplitByPage = json_object_get_boolean_member(root, "wantSplitByPage");

	ret.metadata = json_object_get_object_member(root, "metadata");
	if (!ret.metadata) fail(out, "Input JSON is missing object 'metadata'");

	return ret;
}

void
add_address_list(JsonObject* metadata, const char* key, InternetAddressList* address_list)
{
	if (address_list == NULL) return;

	char* s = internet_address_list_to_string(address_list, g_mime_format_options_get_default(), FALSE);
	if (s == NULL) return;
	json_object_set_string_member(metadata, key, s);
	g_free(s);
}

JsonObject*
create_output_json(InputJson* input_json, GMimeMessage* message)
{
	JsonObject* ret = json_object_new();

	json_object_set_string_member(ret, "filename", input_json->filename);
	json_object_set_boolean_member(ret, "wantOcr", input_json->wantOcr);
	json_object_set_boolean_member(ret, "wantSplitByPage", input_json->wantSplitByPage);
	json_object_set_string_member(ret, "languageCode", input_json->languageCode);

	JsonObject* metadata = json_object_new();

	json_object_foreach_member(input_json->metadata, dup_json_object_child, metadata);

	add_address_list(metadata, "To", g_mime_message_get_to(message));
	add_address_list(metadata, "Reply-To", g_mime_message_get_reply_to(message));
	add_address_list(metadata, "From", g_mime_message_get_from(message));
	add_address_list(metadata, "Cc", g_mime_message_get_cc(message));
	add_address_list(metadata, "Bcc", g_mime_message_get_bcc(message));

	GDateTime* date = g_mime_message_get_date(message);
	if (date) {
		GDateTime* date_utc = g_date_time_to_utc(date);
		gchar* date_string = g_date_time_format(date_utc, "%FT%TZ");
		json_object_set_string_member(metadata, "Date", date_string);
		g_free(date_string);
		g_date_time_unref(date_utc);
		g_date_time_unref(date);
	}

	json_object_set_string_member(metadata, "Subject", g_mime_message_get_subject(message));
	json_object_set_string_member(metadata, "Message-ID", g_mime_message_get_message_id(message));
	json_object_set_string_member(metadata, "In-Reply-To", g_mime_object_get_header(GMIME_OBJECT(message), "In-Reply-To"));
	json_object_set_string_member(metadata, "References", g_mime_object_get_header(GMIME_OBJECT(message), "References"));

	json_object_set_object_member(ret, "metadata", metadata);

	return ret;
}

/**
 * Writes stdin to "input.blob", and returns that file as a stream.
 */
GMimeStream*
open_stdin_as_buffered_blob(Out* out)
{
	GMimeStream* in = g_mime_stream_pipe_new(STDIN_FILENO);
	GMimeStream* blob = g_mime_stream_file_open("input.blob", "w+", NULL);

	if (!out) fail(out, "Could not open input.blob for write");

	gint64 n = g_mime_stream_write_to_stream(in, blob);
	if (n == -1) fail(out, "Could not stream input to input.blob");

	g_object_unref(in);

	g_mime_stream_seek(blob, 0L, GMIME_STREAM_SEEK_SET);
	return blob;
}

GPtrArray*
collect_children(GMimeMessage* message)
{
	GPtrArray* ret = g_ptr_array_new();
	GMimePart* best_body = NULL;

	GMimePartIter* iter = g_mime_part_iter_new(GMIME_OBJECT(message));
	while (g_mime_part_iter_next(iter)) {
		GMimeObject* current = g_mime_part_iter_get_current(iter);
		if (!GMIME_IS_PART(current)) continue;
		GMimePart* part = (GMimePart*) current;

		if (g_mime_part_is_attachment(part)) {
			g_ptr_array_add(ret, part);
		} else {
			GMimeContentType* content_type = g_mime_object_get_content_type(GMIME_OBJECT(part));
			if (content_type == NULL) continue;
			if (
				g_mime_content_type_is_type(content_type, "text", "plain") ||
				g_mime_content_type_is_type(content_type, "text", "html") ||
				g_mime_content_type_is_type(content_type, "application", "rtf")
		        ) {
		        	best_body = part;
			} else {
				g_ptr_array_add(ret, part);
			}
		}
	}
	
	/* non-multipart email: the message's root is its body */
	if (best_body == NULL) {
		GMimeObject* root = g_mime_message_get_mime_part(message);
		if (GMIME_IS_PART(root)) {
			best_body = GMIME_PART(root);
		}
	}

	if (best_body != NULL) {
		g_ptr_array_add(ret, best_body);
	}

	return ret;
}

void
stream_blob(Out* out, int index, GMimePart* part)
{
	begin_child_mime_part(out, index, ".blob");

	GMimeDataWrapper* content = g_mime_part_get_content(part);
	gint64 n_written = g_mime_data_wrapper_write_to_stream(content, out->stream);

	if (n_written == -1) fail(out, "Failed to write blob");
}

void
write_progress(Out* out, int n_processed, int n_total)
{
	begin_mime_part(out, "progress");
	g_mime_stream_printf(out->stream, "{\"children\":{\"nProcessed\":%d,\"nTotal\":%d}}", n_processed, n_total);
}

void
stream_attachment(Out* out, JsonObject* output_json, int index, int n_children, GMimePart* part)
{
	const char* filename = g_mime_part_get_filename(part);
	if (!filename) {
		filename = DEFAULT_FILENAME;
	}

	write_output_json(out, index, filename, DEFAULT_CONTENT_TYPE, output_json);
	stream_blob(out, index, part);
	write_progress(out, index + 1, n_children);
}

void
stream_body(Out* out, JsonObject* output_json, int index, int n_children, GMimePart* part)
{
	GMimeContentType* content_type = g_mime_object_get_content_type(GMIME_OBJECT(part));
	char* content_type_string = g_mime_content_type_get_mime_type(content_type);
	write_output_json(out, index, NULL, content_type_string, output_json);
	free(content_type_string);

	stream_blob(out, index, part);
	write_progress(out, index + 1, n_children);
}

void
stream_child(Out* out, JsonObject* output_json, int index, int n_children, GMimePart* part)
{
	if (g_mime_part_is_attachment((GMimePart*) part)) {
		stream_attachment(out, output_json, index, n_children, part);
	} else {
		stream_body(out, output_json, index, n_children, part);
	}
}

void
stream_children(Out* out, JsonObject* output_json, GPtrArray* children)
{
	guint len = children->len;
	for (guint i = 0; i < len; i++) {
		GMimePart* part = GMIME_PART(children->pdata[i]);
		stream_child(out, output_json, (int) i, (int) len, part);
	}
}

void
print_mime_warning(gint64 offset, GMimeParserWarning errcode, const gchar* item, gpointer user_data)
{
	Out* out = user_data;

	const char* message;
	switch (errcode) {
		case GMIME_WARN_DUPLICATED_HEADER: message = "GMIME_WARN_DUPLICATED_HEADER"; break;
		case GMIME_WARN_DUPLICATED_PARAMETER: return; // seen 2019-10-01 from .pst files
		case GMIME_WARN_UNENCODED_8BIT_HEADER: message = "GMIME_WARN_UNENCODED_8BIT_HEADER"; break;
		case GMIME_WARN_INVALID_CONTENT_TYPE: message = "GMIME_WARN_INVALID_CONTENT_TYPE"; break;
		case GMIME_WARN_INVALID_RFC2047_HEADER_VALUE: message = "GMIME_WARN_INVALID_RFC2047_HEADER_VALUE"; break;
		case GMIME_WARN_MALFORMED_MULTIPART: message = "GMIME_WARN_MALFORMED_MULTIPART"; break;
		case GMIME_WARN_TRUNCATED_MESSAGE: message = "GMIME_WARN_TRUNCATED_MESSAGE"; break;
		case GMIME_WARN_MALFORMED_MESSAGE: message = "GMIME_WARN_MALFORMED_MESSAGE"; break;
		case GMIME_WARN_INVALID_PARAMETER: message = "GMIME_WARN_INVALID_PARAMETER"; break;
		case GMIME_CRIT_INVALID_HEADER_NAME: message = "GMIME_CRIT_INVALID_HEADER_NAME"; break;
		case GMIME_CRIT_CONFLICTING_HEADER: message = "GMIME_CRIT_CONFLICTING_HEADER"; break;
		case GMIME_CRIT_CONFLICTING_PARAMETER: message = "GMIME_CRIT_CONFLICTING_PARAMETER"; break;
		case GMIME_CRIT_MULTIPART_WITHOUT_BOUNDARY: message = "GMIME_CRIT_MULTIPART_WITHOUT_BOUNDARY"; break;
		default: message = "unknown error";
	}

	fail(out, message);
}

GMimeMessage*
load_mime_message_from_stdin(Out* out)
{
	GMimeStream* stream = open_stdin_as_buffered_blob(out);
	if (!stream) fail(out, "Could not open input.blob for reading");
	GMimeParser* mime_parser = g_mime_parser_new_with_stream(stream);
	g_object_unref(stream);

	GMimeParserOptions *mime_options = g_mime_parser_options_new();
	g_mime_parser_options_set_allow_addresses_without_domain(mime_options, TRUE);
	g_mime_parser_options_set_warning_callback(mime_options, print_mime_warning, out);
	GMimeMessage* mime_message = g_mime_parser_construct_message(mime_parser, mime_options);
	g_mime_parser_options_free(mime_options);
	g_object_unref(mime_parser);

	if (!mime_message) fail(out, "invalid email file");
	return mime_message;
}

int
main(int argc, char** argv)
{
	g_mime_init();

	Out out;
	out.mime_boundary = argv[1];
	out.stream = g_mime_stream_pipe_new(STDOUT_FILENO);

	const char* input_json_string = argv[2];
	InputJson input_json = load_input_json(&out, input_json_string);

	GMimeMessage* mime_message = load_mime_message_from_stdin(&out);

	JsonObject* output_json = create_output_json(&input_json, mime_message);

	GPtrArray* children = collect_children(mime_message);
	stream_children(&out, output_json, children);

	begin_mime_part(&out, "done");
	end_mime_message(&out);
	g_object_unref(out.stream);

	return 0;

	//g_ptr_array_free(children, TRUE);
	//json_object_unref(output_json);
	//g_object_unref(mime_message);
	//g_object_unref(input_json.parser);
}
