from distutils.core import setup, Extension

INSTALL_DIR = './live'
module = Extension('live555',
                   include_dirs=['%s/%s/include' % (INSTALL_DIR, x) for x in ['liveMedia', 'BasicUsageEnvironment', 'UsageEnvironment', 'groupsock']],
                   libraries=['liveMedia', 'groupsock', 'BasicUsageEnvironment', 'UsageEnvironment'],
                   #extra_compile_args = ['-fPIC'],
                   library_dirs=['%s/%s' % (INSTALL_DIR, x) for x in ['liveMedia', 'UsageEnvironment', 'groupsock']],
                   sources = ['module.cpp'])
  
setup(name = 'live555',
      version = '1.0',
      description = 'Basic wrapper around live555 to load RTSP video streams',
      ext_modules = [module])
