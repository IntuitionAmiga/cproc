#if __has_embed("preprocess-embed.bin") != __STDC_EMBED_FOUND__
#error resource query failed
#endif
unsigned char data[] = {
#define EMBED_FILE "preprocess-embed.bin"
#define EMBED_LIMIT limit(1 ? 2 : 0)
#define EMBED_PREFIX prefix(9,)
#define EMBED_SUFFIX suffix(,8)
#embed EMBED_FILE EMBED_LIMIT EMBED_PREFIX EMBED_SUFFIX
};
#if __has_embed("preprocess-embed-empty.bin") != __STDC_EMBED_EMPTY__
#error empty resource query failed
#endif
unsigned char empty[] = {
#embed "preprocess-embed-empty.bin" if_empty(7)
};
