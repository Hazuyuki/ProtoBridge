"""Native operation semantics and packet-path regression checks."""
import resource
import subprocess

import pytest

from test_ibgda import run, BINARIES, ROOT, pytestmark  # noqa: F401

BASE = ["--numNodes=2", "--gpusPerNode=2", "--interNodeTopology=host",
        "--interNodeFabricType=RoCE", "--interNodeLatency=0", "--collective=rdma",
        "--algorithm=p2p", "--rdmaSubmission=detailed"]
OPCODES = ["write", "read", "send", "write_imm", "send_imm", "fetch_add",
           "compare_swap", "put_signal", "signal"]


@pytest.mark.parametrize("actor", ["cpu", "gpu"])
@pytest.mark.parametrize("path", ["host", "gpudirect", "staged"])
@pytest.mark.parametrize("opcode", OPCODES)
def test_operations_with_independent_initiator_and_memory_paths(actor, path, opcode):
    fields = run(*BASE, f"--rdmaInitiator={actor}", f"--rdmaDataPath={path}",
                 f"--rdmaOperation={opcode}", "--dataSize=3017", "--p2pIterations=3",
                 "--rdmaStagingChunkBytes=1024", "--rdmaStagingBufferBytes=2048")
    assert fields["rdmaOperationsCompleted"] == fields["rdmaCompletedRequests"] == "3"
    assert fields["rdmaFailedRequests"] == fields["rdmaFailedWqes"] == "0"
    scalar = opcode in {"fetch_add", "compare_swap", "signal"}
    data_wrs = 9 if path == "staged" and not scalar else 3
    expected_wrs = data_wrs + (3 if opcode == "put_signal" else 0)
    assert int(fields["rdmaSubmittedWqes"]) == int(fields["rdmaCompletedWqes"]) == expected_wrs
    expected_receive = data_wrs if opcode in {"send", "send_imm", "write_imm"} else 0
    assert int(fields["rdmaReceiveCompletions"]) == expected_receive
    if opcode == "read":
        assert fields["rdmaReadResponseBytes"] == str(3017 * 3)
        assert int(fields["rdmaReadRequests"]) == data_wrs
    if opcode in {"fetch_add", "compare_swap"}:
        assert fields["rdmaAtomicExecutions"] == "3"
    if opcode == "fetch_add":
        assert fields["rdmaRemoteValue"] == "3"
    if opcode in {"put_signal", "signal"}:
        assert fields["rdmaSignalUpdates"] == "3" and fields["rdmaSignalValue"] == "1"


@pytest.mark.parametrize("opcode", ["send", "send_imm", "write_imm"])
def test_delayed_receive_posting_causes_rnr_then_succeeds(opcode):
    fields = run(*BASE, f"--rdmaOperation={opcode}", "--dataSize=9000",
                 "--rdmaReceivePostDelayNs=5000", "--rdmaRnrRetryNs=1000", "--rdmaRnrRetryLimit=20")
    assert int(fields["rdmaRnrRetries"]) > 0
    assert fields["rdmaReceiveCompletions"] == "1"
    assert fields["rdmaRnrExhausted"] == "0"


def test_rnr_exhaustion_returns_failed_wr_and_incomplete_workload():
    result = subprocess.run([str(BINARIES[0]), *BASE, "--rdmaOperation=send", "--dataSize=64",
                             "--rdmaReceivePostDelayNs=100000", "--rdmaRnrRetryLimit=2"],
                            cwd=ROOT, text=True, capture_output=True, timeout=10,
                            preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    assert result.returncode == 0, result.stderr
    fields = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
    assert fields["status"] == "incomplete"
    assert fields["rdmaOperationsFailed"] == fields["rdmaFailedRequests"] == "1"
    assert fields["rdmaRnrExhausted"] == "1"
    assert fields["rdmaCompletedWqes"] == "0"


@pytest.mark.parametrize("opcode", ["read", "send", "fetch_add", "put_signal"])
def test_reliable_packet_path_recovers_operation_requests_and_responses(opcode):
    fields = run(*BASE, f"--rdmaOperation={opcode}", "--dataSize=8192", "--p2pIterations=32",
                 "--llrEnabled=1", "--llrMode=sack", "--packetLossRate=0.03",
                 "--llrTimeoutRetransmit=1", "--llrRetryTimeout=200000", "--llrRetryLimit=50",
                 "--soTxWindow=4")
    assert fields["rdmaOperationsCompleted"] == "32"
    assert int(fields["retransmittedPackets"]) > 0
    assert fields["permanentLosses"] == "0"
    if opcode == "fetch_add":
        assert fields["rdmaRemoteValue"] == fields["rdmaAtomicExecutions"] == "32"
    if opcode == "put_signal":
        assert fields["rdmaSignalUpdates"] == "32"


def test_read_and_atomic_responder_costs_do_not_change_request_startup():
    for opcode, option in [("read", "rdmaReadServiceNs"), ("fetch_add", "rdmaAtomicServiceNs")]:
        before = run(*BASE, f"--rdmaOperation={opcode}", "--dataSize=8")
        after = run(*BASE, f"--rdmaOperation={opcode}", "--dataSize=8", f"--{option}=1234")
        assert before["rdmaMeanStartupNs"] == after["rdmaMeanStartupNs"]
        # The request ACK and response share a link; their 1-ns overlap can disappear.
        assert abs(int(after["simTimeNs"]) - int(before["simTimeNs"]) - 1234) <= 1


def test_read_staging_uses_reverse_direction_and_bounded_buffers():
    args = [*BASE, "--rdmaOperation=read", "--rdmaDataPath=staged", "--dataSize=4096",
            "--rdmaStagingChunkBytes=1024", "--rdmaD2hNs=1000", "--rdmaH2dNs=1000"]
    single = run(*args, "--rdmaStagingBufferBytes=1024")
    double = run(*args, "--rdmaStagingBufferBytes=2048")
    assert int(double["simTimeNs"]) < int(single["simTimeNs"])
    assert double["rdmaSourceD2hBytes"] == double["rdmaDestinationH2dBytes"] == "4096"
    assert int(double["rdmaSourceStagingPeakBytes"]) <= 2048
    assert int(double["rdmaDestinationStagingPeakBytes"]) <= 2048


@pytest.mark.parametrize("name,opcode", [("rdma-read", "read"), ("rdma-send", "send"),
                                        ("rdma-atomic", "fetch_add"), ("rdma-put-signal", "put_signal")])
def test_operation_presets_are_runnable(name, opcode):
    fields = run(f"--protocolConfig=configs/protocol_configs/{name}.cfg", "--numNodes=2",
                 "--gpusPerNode=2", "--dataSize=9000")
    assert fields["rdmaOperation"] == opcode
    assert fields["rdmaOperationsCompleted"] == "1"


def test_full_uint64_fetch_add_wraps_and_returns_exact_old_word():
    fields = run(*BASE, "--rdmaOperation=fetch_add", "--rdmaInitialValue=18446744073709551614",
                 "--rdmaOperand=2")
    assert fields["rdmaLastAtomicValue"] == "18446744073709551614"
    assert fields["rdmaLastAtomicValueHex"] == "0xfffffffffffffffe"
    assert fields["rdmaRemoteValue"] == "0"


@pytest.mark.parametrize("flags,message", [
    (["--rdmaOperation=bogus"], "rdmaOperation"),
    (["--rdmaOperation=read", "--rdmaMaxReadAtomic=0"], "rdmaMaxReadAtomic"),
    (["--rdmaOperation=fetch_add", "--rdmaRemoteAddress=1"], "aligned"),
    (["--rdmaOperation=put_signal", "--rdmaSignalAddress=0"], "overlap"),
    (["--rdmaRnrRetryNs=0"], "rdmaRnrRetryNs"),
])
def test_invalid_operation_settings_fail_explicitly(flags, message):
    output = run(*BASE, "--dataSize=64", *flags, success=False)
    assert message.lower() in output.lower()
