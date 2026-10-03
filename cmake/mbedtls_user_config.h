// mbedtls_user_config.h -- what the programs' WebRTC (libdatachannel) takes of
// mbedTLS besides its defaults (cmake/webrtc.cmake): DTLS's SRTP extension,
// which its DTLS transport is built with
#define MBEDTLS_SSL_DTLS_SRTP
