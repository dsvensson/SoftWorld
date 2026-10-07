// mbedtls_user_config.h -- what the programs take of mbedTLS besides its
// defaults (cmake/mbedtls, and cmake/webrtc with it): DTLS's SRTP extension,
// which WebRTC's DTLS transport (libdatachannel's) is built with. The same
// with WebRTC or without, as a program has one mbedTLS, its HTTPS's too.
#define MBEDTLS_SSL_DTLS_SRTP
