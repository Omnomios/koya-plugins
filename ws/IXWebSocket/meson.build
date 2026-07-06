openssl_dep = dependency('openssl', required: true)

ix_inc = include_directories(
  '.',
  'third_party/msgpack11',
)

ixwebsocket_sources = files(
  'ixwebsocket/IXBench.cpp',
  'ixwebsocket/IXCancellationRequest.cpp',
  'ixwebsocket/IXConnectionState.cpp',
  'ixwebsocket/IXDNSLookup.cpp',
  'ixwebsocket/IXExponentialBackoff.cpp',
  'ixwebsocket/IXGetFreePort.cpp',
  'ixwebsocket/IXGzipCodec.cpp',
  'ixwebsocket/IXHttp.cpp',
  'ixwebsocket/IXHttpClient.cpp',
  'ixwebsocket/IXHttpServer.cpp',
  'ixwebsocket/IXNetSystem.cpp',
  'ixwebsocket/IXSelectInterrupt.cpp',
  'ixwebsocket/IXSelectInterruptFactory.cpp',
  'ixwebsocket/IXSelectInterruptPipe.cpp',
  'ixwebsocket/IXSelectInterruptEvent.cpp',
  'ixwebsocket/IXSetThreadName.cpp',
  'ixwebsocket/IXSocket.cpp',
  'ixwebsocket/IXSocketConnect.cpp',
  'ixwebsocket/IXSocketFactory.cpp',
  'ixwebsocket/IXSocketServer.cpp',
  'ixwebsocket/IXSocketTLSOptions.cpp',
  'ixwebsocket/IXStrCaseCompare.cpp',
  'ixwebsocket/IXUdpSocket.cpp',
  'ixwebsocket/IXUrlParser.cpp',
  'ixwebsocket/IXUuid.cpp',
  'ixwebsocket/IXUserAgent.cpp',
  'ixwebsocket/IXWebSocket.cpp',
  'ixwebsocket/IXWebSocketCloseConstants.cpp',
  'ixwebsocket/IXWebSocketHandshake.cpp',
  'ixwebsocket/IXWebSocketHttpHeaders.cpp',
  'ixwebsocket/IXWebSocketPerMessageDeflate.cpp',
  'ixwebsocket/IXWebSocketPerMessageDeflateCodec.cpp',
  'ixwebsocket/IXWebSocketPerMessageDeflateOptions.cpp',
  'ixwebsocket/IXWebSocketProxyServer.cpp',
  'ixwebsocket/IXWebSocketServer.cpp',
  'ixwebsocket/IXWebSocketTransport.cpp',
  'ixwebsocket/IXSocketOpenSSL.cpp',
)

ixwebsocket_lib = static_library(
  'ixwebsocket',
  ixwebsocket_sources,
  include_directories: ix_inc,
  dependencies: [threads_dep, openssl_dep],
  cpp_args: [
    '-DIXWEBSOCKET_USE_TLS',
    '-DIXWEBSOCKET_USE_OPEN_SSL',
  ],
  pic: true,
)

ixwebsocket_dep = declare_dependency(
  link_with: ixwebsocket_lib,
  include_directories: ix_inc,
  dependencies: [threads_dep, openssl_dep],
)
