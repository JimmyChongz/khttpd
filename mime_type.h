#ifndef _MIME_TYPE_H_
#define _MIME_TYPE_H_

typedef struct {
    const char *type;
    const char *string;
} mime_map;

const char *get_mime_str(const char *request_url);
extern const mime_map mime_types[];

#endif