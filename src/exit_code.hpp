#pragma once

namespace aes67_srt {

/**
 * Process exit codes, shared by both front-ends.
 *
 * They are the appliance's, and the Mac endpoint uses the same numbers so a
 * script, a systemd unit or a person sees one vocabulary: 0 is fine, 2 is a bad
 * command line, 3 is a configuration that will not do, 4 is a run that failed.
 */
enum class ExitCode { ok = 0, usage = 2, config_error = 3, runtime_error = 4 };

}  // namespace aes67_srt
