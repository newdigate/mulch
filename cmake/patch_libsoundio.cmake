# Patches libsoundio 2.0.0 at fetch time: CMakeLists.txt runs this as libsoundio's FetchContent
# PATCH_COMMAND, with libsoundio's source directory as the working directory.
#
# soundio_alsa_init() marks its inotify descriptor not-open (-1) but not its notify pipe, which
# stays {0, 0} from the zero-filled context until pipe2() is reached. When the backend fails
# before that -- inotify_add_watch("/dev/snd") fails on a machine with no sound devices, and
# soundio_connect() moves on to the Dummy backend -- destroy_alsa() closes both ends of that
# pipe, that is, file descriptor 0: stdin the first time, then whatever file the process opened
# next (on the Linux CI, a live Recorder's mp4, whose trailer then failed with EBADF). Marking the
# pipe -1 too makes those two close() calls fail harmlessly. Upstream master (49a1f78, 2023) still
# has the bug.
#
# Safe to run again (the fix is found already there), and it stops the configure when the source
# no longer has the lines it expects, so a libsoundio bump cannot drop the fix silently. file(READ)
# drops carriage returns, so a Windows checkout with CRLF endings matches too; it is written back
# with LF endings, which is harmless: alsa.c is only compiled on Linux.
set(_file "src/alsa.c")
file(READ "${_file}" _src)
set(_old "    sia->notify_fd = -1;\n    sia->notify_wd = -1;\n")
set(_new "${_old}    sia->notify_pipe_fd[0] = -1;\n    sia->notify_pipe_fd[1] = -1;\n")

string(FIND "${_src}" "${_new}" _patched)
string(FIND "${_src}" "${_old}" _at)
if(_patched GREATER -1)
  # already patched: nothing to do
elseif(_at EQUAL -1)
  message(FATAL_ERROR "patch_libsoundio.cmake: ${_file} no longer has the soundio_alsa_init() "
                      "lines this patch expects; check whether the notify_pipe_fd fix is still needed")
else()
  string(REPLACE "${_old}" "${_new}" _src "${_src}")
  file(WRITE "${_file}" "${_src}")
  message(STATUS "Patched libsoundio: soundio_alsa_init() marks its notify pipe not-open")
endif()
