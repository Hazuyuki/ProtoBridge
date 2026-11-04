/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IBGDA_SUBMISSION_MODEL_H
#define IBGDA_SUBMISSION_MODEL_H
// Compatibility names for the original GPU-only interface.
#include "rdma-submission-model.h"

namespace ns3
{
using IbgdaConfig = RdmaConfig;
using IbgdaWorkTiming = RdmaWorkTiming;
using IbgdaStats = RdmaStats;
using IbgdaPacketTag = RdmaPacketTag;
using IbgdaSubmissionModel = RdmaSubmissionModel;

inline void
AddIbgdaCommandLine(CommandLine& cmd, std::string& mode, RdmaConfig& c)
{
    AddRdmaCommandLine(cmd, mode, c);
}

inline void
ApplyIbgdaProfile(const std::unordered_map<std::string, std::string>& v,
                  std::string& mode,
                  RdmaConfig& c)
{
    ApplyRdmaProfile(v, mode, c);
}
} // namespace ns3
#endif
