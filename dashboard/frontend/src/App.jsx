import { useEffect, useMemo, useState } from "react";

import {
  Activity,
  BarChart3,
  Binary,
  Box,
  Braces,
  CheckCircle2,
  ChevronRight,
  CircleGauge,
  Cpu,
  Database,
  FlaskConical,
  Gauge,
  GitBranch,
  HardDrive,
  Layers3,
  MemoryStick,
  Menu,
  Play,
  RefreshCw,
  Server,
  ShieldCheck,
  TerminalSquare,
  Timer,
  TrendingUp,
  X,
  Zap,
} from "lucide-react";

import {
  Bar,
  BarChart,
  CartesianGrid,
  Legend,
  Line,
  LineChart,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from "recharts";

import "./index.css";


const API = "http://127.0.0.1:5050/api";


const NAV_ITEMS = [
  { id: "dashboard", label: "Dashboard", icon: CircleGauge },
  { id: "benchmarks", label: "Benchmark Lab", icon: Activity },
  { id: "optimization", label: "Optimization", icon: Zap },
  { id: "adaptive", label: "Adaptive Engine", icon: Braces },
  { id: "syscalls", label: "Syscall Analyzer", icon: Binary },
  { id: "scheduler", label: "CPU Scheduler", icon: Cpu },
  { id: "memory", label: "Virtual Memory", icon: MemoryStick },
  { id: "integrity", label: "Integrity", icon: ShieldCheck },
  { id: "evidence", label: "Evidence", icon: GitBranch },
];


function number(value, digits = 2) {
  const parsed = Number(value);

  if (!Number.isFinite(parsed)) {
    return "—";
  }

  return parsed.toLocaleString(undefined, {
    minimumFractionDigits: digits,
    maximumFractionDigits: digits,
  });
}


function percentChange(before, after, lowerIsBetter = true) {
  const a = Number(before);
  const b = Number(after);

  if (!Number.isFinite(a) || !Number.isFinite(b) || a === 0) {
    return 0;
  }

  return lowerIsBetter
    ? ((a - b) / a) * 100
    : ((b - a) / a) * 100;
}


function findBy(rows, key, value) {
  return rows?.find((row) => row[key] === value);
}


function StatusDot({ online }) {
  return (
    <span className={`status-dot ${online ? "online" : "offline"}`} />
  );
}


function StatCard({
  icon: Icon,
  label,
  value,
  note,
  tone = "normal",
}) {
  return (
    <article className={`stat-card ${tone}`}>
      <div className="stat-top">
        <div className="stat-icon">
          <Icon size={19} />
        </div>
        <span className="stat-label">{label}</span>
      </div>

      <div className="stat-value">{value}</div>

      <div className="stat-note">{note}</div>
    </article>
  );
}


function Panel({ title, subtitle, children, action }) {
  return (
    <section className="panel">
      <div className="panel-header">
        <div>
          <h3>{title}</h3>
          {subtitle && <p>{subtitle}</p>}
        </div>

        {action}
      </div>

      <div className="panel-body">
        {children}
      </div>
    </section>
  );
}


function Badge({ children, type = "neutral" }) {
  return (
    <span className={`badge ${type}`}>
      {children}
    </span>
  );
}


function ChartTooltip({ active, payload, label }) {
  if (!active || !payload?.length) {
    return null;
  }

  return (
    <div className="chart-tooltip">
      <strong>{label}</strong>

      {payload.map((item) => (
        <div key={item.dataKey}>
          {item.name}: {number(item.value, 3)}
        </div>
      ))}
    </div>
  );
}


function ExperimentForm({
  title,
  description,
  command,
  fields,
  onRun,
  running,
}) {
  const initial = {};

  fields.forEach((field) => {
    initial[field.key] = field.defaultValue;
  });

  const [values, setValues] = useState(initial);

  function submit(event) {
    event.preventDefault();

    onRun(
      command,
      fields.map((field) => values[field.key])
    );
  }

  return (
    <form className="experiment-form" onSubmit={submit}>
      <div className="experiment-form-title">
        <div className="experiment-icon">
          <FlaskConical size={19} />
        </div>

        <div>
          <h4>{title}</h4>
          <p>{description}</p>
        </div>
      </div>

      <div className="form-fields">
        {fields.map((field) => (
          <label key={field.key}>
            <span>{field.label}</span>

            {field.type === "select" ? (
              <select
                value={values[field.key]}
                onChange={(event) => {
                  setValues((previous) => ({
                    ...previous,
                    [field.key]: event.target.value,
                  }));
                }}
              >
                {field.options.map((option) => (
                  <option key={option.value} value={option.value}>
                    {option.label}
                  </option>
                ))}
              </select>
            ) : (
              <input
                type="number"
                min="1"
                value={values[field.key]}
                onChange={(event) => {
                  setValues((previous) => ({
                    ...previous,
                    [field.key]: event.target.value,
                  }));
                }}
              />
            )}
          </label>
        ))}
      </div>

      <button
        type="submit"
        className="run-button"
        disabled={running}
      >
        {running ? (
          <>
            <RefreshCw size={17} className="spin" />
            Running
          </>
        ) : (
          <>
            <Play size={17} fill="currentColor" />
            Run experiment
          </>
        )}
      </button>
    </form>
  );
}


function ConsoleDrawer({ data, running, onClose }) {
  if (!data && !running) {
    return null;
  }

  return (
    <aside className="console-drawer">
      <div className="console-header">
        <div className="console-title">
          <TerminalSquare size={18} />

          <div>
            <strong>FastIPC-X Console</strong>
            <span>
              {running ? "Experiment running" : "Execution finished"}
            </span>
          </div>
        </div>

        {!running && (
          <button className="icon-button" onClick={onClose}>
            <X size={18} />
          </button>
        )}
      </div>

      <div className="console-command">
        <span>$</span>{" "}
        {data?.command || "Preparing command..."}
      </div>

      <pre>
        {running
          ? "Executing native FastIPC-X backend...\nPlease wait."
          : `${data?.stdout || ""}${data?.stderr || ""}`}
      </pre>

      {!running && data && (
        <div
          className={`console-status ${
            data.exit_code === 0 ? "success" : "error"
          }`}
        >
          Exit code: {data.exit_code}
        </div>
      )}
    </aside>
  );
}


function DashboardPage({ data }) {
  const benchmark = data.benchmark || [];
  const sync = data.shm_optimization || [];
  const syscall = data.syscalls || [];
  const integrity = data.integrity || [];

  const baselineWinner = [...benchmark].sort(
    (a, b) => Number(a.median_ms) - Number(b.median_ms)
  )[0];

  const shmBaseline = findBy(sync, "variant", "baseline");
  const ring = findBy(sync, "variant", "ringbuffer");

  const latencyReduction = percentChange(
    shmBaseline?.median_ms,
    ring?.median_ms,
    true
  );

  const throughputGain = percentChange(
    shmBaseline?.median_throughput_mbps,
    ring?.median_throughput_mbps,
    false
  );

  const syscallReduction = findBy(
    syscall,
    "metric",
    "total_syscalls"
  );

  const passes = integrity.filter(
    (row) => row.result === "PASS"
  ).length;

  const benchmarkChart = benchmark.map((row) => ({
    method: row.method,
    latency: Number(row.median_ms),
    throughput: Number(row.median_throughput_mbps),
  }));

  const syncChart = sync.map((row) => ({
    mode: row.variant === "ringbuffer" ? "SHM-RING" : "Baseline SHM",
    latency: Number(row.median_ms),
    throughput: Number(row.median_throughput_mbps),
  }));

  return (
    <>
      <section className="hero">
        <div>
          <div className="eyebrow">
            <span className="pulse-dot" />
            Native OS performance engine online
          </div>

          <h1>
            Adaptive IPC
            <span> Optimization Engine</span>
          </h1>

          <p>
            Benchmark, profile and optimize Linux inter-process
            communication using real kernel-level measurements.
          </p>
        </div>

        <div className="hero-chip">
          <Server size={18} />
          C11 / POSIX / WSL2
        </div>
      </section>

      <div className="stats-grid">
        <StatCard
          icon={Gauge}
          label="Baseline Winner"
          value={baselineWinner?.method || "—"}
          note={
            baselineWinner
              ? `${number(baselineWinner.median_ms, 3)} ms at 100 MB`
              : "No baseline data"
          }
        />

        <StatCard
          icon={Zap}
          label="SHM-RING Latency"
          value={
            ring
              ? `${number(ring.median_ms, 3)} ms`
              : "—"
          }
          note={`${number(latencyReduction)}% lower than baseline SHM`}
          tone="accent"
        />

        <StatCard
          icon={TrendingUp}
          label="Throughput Gain"
          value={`${number(throughputGain)}%`}
          note={
            ring
              ? `${number(ring.median_throughput_mbps, 0)} MB/s`
              : "No ring result"
          }
        />

        <StatCard
          icon={Binary}
          label="Syscall Reduction"
          value={
            syscallReduction
              ? `${number(syscallReduction.reduction_percent)}%`
              : "—"
          }
          note="Optimized SHM vs baseline"
        />

        <StatCard
          icon={ShieldCheck}
          label="Integrity"
          value={`${passes}/${integrity.length || 0} PASS`}
          note="Recorded checksum verification"
          tone="success"
        />
      </div>

      <div className="two-column">
        <Panel
          title="Baseline IPC Latency"
          subtitle="100 MB payload · median latency"
        >
          <div className="chart-box">
            <ResponsiveContainer width="100%" height="100%">
              <BarChart data={benchmarkChart}>
                <CartesianGrid vertical={false} strokeDasharray="3 3" />
                <XAxis dataKey="method" />
                <YAxis />
                <Tooltip content={<ChartTooltip />} />
                <Bar
                  dataKey="latency"
                  name="Latency (ms)"
                  radius={[6, 6, 0, 0]}
                  fill="var(--chart-a)"
                />
              </BarChart>
            </ResponsiveContainer>
          </div>
        </Panel>

        <Panel
          title="Synchronization Optimization"
          subtitle="Baseline SHM vs production SHM-RING"
        >
          <div className="chart-box">
            <ResponsiveContainer width="100%" height="100%">
              <BarChart data={syncChart}>
                <CartesianGrid vertical={false} strokeDasharray="3 3" />
                <XAxis dataKey="mode" />
                <YAxis />
                <Tooltip content={<ChartTooltip />} />
                <Bar
                  dataKey="latency"
                  name="Latency (ms)"
                  radius={[6, 6, 0, 0]}
                  fill="var(--chart-b)"
                />
              </BarChart>
            </ResponsiveContainer>
          </div>
        </Panel>
      </div>

      <Panel
        title="Adaptive Decisions"
        subtitle="Method selected for different workload sizes"
      >
        <div className="adaptive-grid">
          {(data.workloads || []).map((row) => (
            <div className="adaptive-card" key={row.payload_mb}>
              <div className="adaptive-size">
                {row.payload_mb}
                <span>MB</span>
              </div>

              <div className="adaptive-details">
                <Badge
                  type={
                    row.selected_method === "SHM-RING"
                      ? "success"
                      : "accent"
                  }
                >
                  {row.selected_method}
                </Badge>

                <strong>{row.chunk_kb} KB chunks</strong>

                <span>
                  {number(row.median_ms, 3)} ms ·{" "}
                  {number(row.median_throughput_mbps, 0)} MB/s
                </span>
              </div>
            </div>
          ))}
        </div>
      </Panel>

      <Panel
        title="Scientific Guardrails"
        subtitle="What the measurements do and do not prove"
      >
        <div className="guardrails">
          <div>
            <Badge type="accent">Measured</Badge>
            <strong>SHM-RING optimization</strong>
            <span>
              Normal repeated benchmarks are primary performance evidence.
            </span>
          </div>

          <div>
            <Badge type="warning">Experimental</Badge>
            <strong>CPU affinity</strong>
            <span>
              Scheduler results depend on hardware, kernel and topology.
            </span>
          </div>

          <div>
            <Badge type="warning">Critical path</Badge>
            <strong>Pre-faulting</strong>
            <span>
              First-touch cost moves into setup rather than disappearing.
            </span>
          </div>
        </div>
      </Panel>
    </>
  );
}


function BenchmarksPage({ data, onRun, running }) {
  const chart = (data.benchmark || []).map((row) => ({
    method: row.method,
    latency: Number(row.median_ms),
    throughput: Number(row.median_throughput_mbps),
  }));

  return (
    <>
      <PageTitle
        eyebrow="IPC LAB"
        title="Benchmark Lab"
        description="Run the native repeated-trial benchmark suite and compare IPC methods."
      />

      <ExperimentForm
        title="Repeated IPC benchmark"
        description="Runs PIPE, FIFO, SOCKET and baseline SHM under the same payload."
        command="benchmark"
        running={running}
        onRun={onRun}
        fields={[
          {
            key: "payload",
            label: "Payload (MB)",
            defaultValue: "100",
          },
          {
            key: "trials",
            label: "Trials",
            defaultValue: "5",
          },
        ]}
      />

      <Panel
        title="Recorded Baseline Results"
        subtitle="Median values from the stored benchmark evidence"
      >
        <div className="chart-box large">
          <ResponsiveContainer width="100%" height="100%">
            <BarChart data={chart}>
              <CartesianGrid vertical={false} strokeDasharray="3 3" />
              <XAxis dataKey="method" />
              <YAxis />
              <Tooltip content={<ChartTooltip />} />
              <Legend />
              <Bar
                dataKey="latency"
                name="Latency (ms)"
                fill="var(--chart-a)"
                radius={[5, 5, 0, 0]}
              />
            </BarChart>
          </ResponsiveContainer>
        </div>
      </Panel>
    </>
  );
}


function OptimizationPage({ data, onRun, running }) {
  const slots = (data.ring_slots || []).map((row) => ({
    slots: Number(row.slot_count),
    latency: Number(row.median_ms),
    throughput: Number(row.median_throughput_mbps),
  }));

  return (
    <>
      <PageTitle
        eyebrow="OPTIMIZER"
        title="Optimization Center"
        description="Tune chunk size, synchronization strategy and SHM ring depth."
      />

      <div className="forms-grid">
        <ExperimentForm
          title="Chunk-size sweep"
          description="Finds the best transfer chunk for each IPC mechanism."
          command="optimize-chunk"
          running={running}
          onRun={onRun}
          fields={[
            {
              key: "payload",
              label: "Payload (MB)",
              defaultValue: "100",
            },
            {
              key: "trials",
              label: "Trials",
              defaultValue: "5",
            },
          ]}
        />

        <ExperimentForm
          title="SHM synchronization"
          description="Compares one-slot SHM against the bounded SHM ring."
          command="optimize-shm"
          running={running}
          onRun={onRun}
          fields={[
            {
              key: "payload",
              label: "Payload (MB)",
              defaultValue: "100",
            },
            {
              key: "chunk",
              label: "Chunk (KB)",
              defaultValue: "64",
            },
            {
              key: "trials",
              label: "Trials",
              defaultValue: "5",
            },
          ]}
        />

        <ExperimentForm
          title="Ring-slot sweep"
          description="Controlled microbenchmark for SHM ring depth."
          command="optimize-ring-slots"
          running={running}
          onRun={onRun}
          fields={[
            {
              key: "payload",
              label: "Payload (MB)",
              defaultValue: "100",
            },
            {
              key: "chunk",
              label: "Chunk (KB)",
              defaultValue: "64",
            },
            {
              key: "trials",
              label: "Trials",
              defaultValue: "5",
            },
          ]}
        />
      </div>

      <Panel
        title="SHM Ring Slot Sweep"
        subtitle="Controlled benchmark result — not automatic production adoption"
        action={<Badge type="warning">Microbenchmark</Badge>}
      >
        <div className="chart-box large">
          <ResponsiveContainer width="100%" height="100%">
            <LineChart data={slots}>
              <CartesianGrid vertical={false} strokeDasharray="3 3" />
              <XAxis dataKey="slots" />
              <YAxis />
              <Tooltip content={<ChartTooltip />} />
              <Line
                dataKey="latency"
                name="Latency (ms)"
                type="monotone"
                stroke="var(--chart-c)"
                strokeWidth={3}
                dot={{ r: 5 }}
              />
            </LineChart>
          </ResponsiveContainer>
        </div>
      </Panel>
    </>
  );
}


function AdaptivePage({ data, onRun, running }) {
  return (
    <>
      <PageTitle
        eyebrow="ADAPTIVE ENGINE"
        title="Workload-Aware IPC Selection"
        description="Use measured profiles to choose an IPC method and transfer configuration."
      />

      <div className="forms-grid two">
        <ExperimentForm
          title="Recommend"
          description="Selects the best measured IPC configuration for a payload."
          command="recommend"
          running={running}
          onRun={onRun}
          fields={[
            {
              key: "payload",
              label: "Payload (MB)",
              defaultValue: "100",
            },
          ]}
        />

        <ExperimentForm
          title="Auto mode"
          description="Loads the adaptive profile and executes the selected strategy."
          command="auto"
          running={running}
          onRun={onRun}
          fields={[
            {
              key: "payload",
              label: "Payload (MB)",
              defaultValue: "100",
            },
          ]}
        />
      </div>

      <Panel
        title="Adaptive Profile"
        subtitle="Selection changes with workload size"
      >
        <div className="data-table-wrap">
          <table className="data-table">
            <thead>
              <tr>
                <th>Payload</th>
                <th>Class</th>
                <th>Selected IPC</th>
                <th>Chunk</th>
                <th>Median</th>
                <th>Throughput</th>
                <th>Profile</th>
              </tr>
            </thead>

            <tbody>
              {(data.workloads || []).map((row) => (
                <tr key={row.payload_mb}>
                  <td>{row.payload_mb} MB</td>
                  <td>{row.workload_class}</td>
                  <td>
                    <Badge
                      type={
                        row.selected_method === "SHM-RING"
                          ? "success"
                          : "accent"
                      }
                    >
                      {row.selected_method}
                    </Badge>
                  </td>
                  <td>{row.chunk_kb} KB</td>
                  <td>{number(row.median_ms, 3)} ms</td>
                  <td>
                    {number(row.median_throughput_mbps, 0)} MB/s
                  </td>
                  <td>{row.profile_source}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </Panel>
    </>
  );
}


function SyscallsPage({ data }) {
  const chart = (data.syscalls || []).map((row) => ({
    metric: row.metric.replaceAll("_", " "),
    baseline: Number(row.baseline),
    optimized: Number(row.optimized),
  }));

  return (
    <>
      <PageTitle
        eyebrow="KERNEL BEHAVIOR"
        title="System Call Analyzer"
        description="Compare syscall and futex activity between baseline SHM and SHM-RING."
      />

      <div className="info-banner">
        <Binary size={20} />
        <div>
          <strong>Tracing is behavioral evidence.</strong>
          <span>
            strace instrumentation changes wall-clock timing, so normal
            repeated benchmarks remain the performance proof.
          </span>
        </div>
      </div>

      <Panel
        title="Syscall Reduction"
        subtitle="Baseline vs optimized SHM"
      >
        <div className="chart-box large">
          <ResponsiveContainer width="100%" height="100%">
            <BarChart data={chart}>
              <CartesianGrid vertical={false} strokeDasharray="3 3" />
              <XAxis dataKey="metric" />
              <YAxis />
              <Tooltip content={<ChartTooltip />} />
              <Legend />
              <Bar
                dataKey="baseline"
                name="Baseline"
                fill="var(--chart-muted)"
                radius={[4, 4, 0, 0]}
              />
              <Bar
                dataKey="optimized"
                name="Optimized"
                fill="var(--chart-a)"
                radius={[4, 4, 0, 0]}
              />
            </BarChart>
          </ResponsiveContainer>
        </div>
      </Panel>

      <div className="metric-list">
        {(data.syscalls || []).map((row) => (
          <div className="metric-row" key={row.metric}>
            <span>{row.metric.replaceAll("_", " ")}</span>
            <strong>
              {row.baseline} → {row.optimized}
            </strong>
            <Badge type="success">
              -{number(row.reduction_percent)}%
            </Badge>
          </div>
        ))}
      </div>
    </>
  );
}


function SchedulerPage({ data, onRun, running }) {
  const chart = (data.affinity || []).map((row) => ({
    mode: row.mode,
    latency: Number(row.median_ms),
    throughput: Number(row.median_throughput_mbps),
  }));

  return (
    <>
      <PageTitle
        eyebrow="SCHEDULER LAB"
        title="CPU Affinity Analysis"
        description="Measure the effect of producer/consumer CPU placement on SHM-RING."
      />

      <ExperimentForm
        title="Affinity experiment"
        description="Compares unpinned, same-CPU and separate-physical-core placement."
        command="analyze-affinity"
        running={running}
        onRun={onRun}
        fields={[
          {
            key: "payload",
            label: "Payload (MB)",
            defaultValue: "100",
          },
          {
            key: "chunk",
            label: "Chunk (KB)",
            defaultValue: "64",
          },
          {
            key: "trials",
            label: "Trials",
            defaultValue: "5",
          },
        ]}
      />

      <Panel
        title="Scheduler Placement"
        subtitle="Result is system-specific and remains experimental"
        action={<Badge type="warning">Experimental</Badge>}
      >
        <div className="chart-box large">
          <ResponsiveContainer width="100%" height="100%">
            <BarChart data={chart}>
              <CartesianGrid vertical={false} strokeDasharray="3 3" />
              <XAxis dataKey="mode" />
              <YAxis />
              <Tooltip content={<ChartTooltip />} />
              <Bar
                dataKey="latency"
                name="Latency (ms)"
                fill="var(--chart-c)"
                radius={[6, 6, 0, 0]}
              />
            </BarChart>
          </ResponsiveContainer>
        </div>
      </Panel>
    </>
  );
}


function MemoryPage({ data, onRun, running }) {
  const timing = (data.memory || []).map((row) => ({
    mode: row.mode,
    setup: Number(row.median_setup_ms),
    timed: Number(row.median_timed_ms),
    total: Number(row.median_total_ms),
  }));

  const faults = (data.memory || []).map((row) => ({
    mode: row.mode,
    setup: Number(row.average_setup_minor_faults),
    timed: Number(row.average_timed_minor_faults),
  }));

  return (
    <>
      <PageTitle
        eyebrow="MEMORY LAB"
        title="Virtual Memory & Page Faults"
        description="Separate critical-path first-touch cost from setup cost."
      />

      <ExperimentForm
        title="Page-fault experiment"
        description="Compares demand paging, pre-faulting and MADV_WILLNEED."
        command="optimize-memory"
        running={running}
        onRun={onRun}
        fields={[
          {
            key: "payload",
            label: "Payload (MB)",
            defaultValue: "100",
          },
          {
            key: "chunk",
            label: "Chunk (KB)",
            defaultValue: "64",
          },
          {
            key: "trials",
            label: "Trials",
            defaultValue: "5",
          },
        ]}
      />

      <div className="two-column">
        <Panel
          title="Timing Breakdown"
          subtitle="Setup vs timed critical path"
        >
          <div className="chart-box">
            <ResponsiveContainer width="100%" height="100%">
              <BarChart data={timing}>
                <CartesianGrid vertical={false} strokeDasharray="3 3" />
                <XAxis dataKey="mode" />
                <YAxis />
                <Tooltip content={<ChartTooltip />} />
                <Legend />
                <Bar
                  dataKey="setup"
                  name="Setup (ms)"
                  stackId="time"
                  fill="var(--chart-muted)"
                />
                <Bar
                  dataKey="timed"
                  name="Timed (ms)"
                  stackId="time"
                  fill="var(--chart-a)"
                />
              </BarChart>
            </ResponsiveContainer>
          </div>
        </Panel>

        <Panel
          title="Minor Page Fault Placement"
          subtitle="Pre-faulting moves first-touch work into setup"
        >
          <div className="chart-box">
            <ResponsiveContainer width="100%" height="100%">
              <BarChart data={faults}>
                <CartesianGrid vertical={false} strokeDasharray="3 3" />
                <XAxis dataKey="mode" />
                <YAxis />
                <Tooltip content={<ChartTooltip />} />
                <Legend />
                <Bar
                  dataKey="setup"
                  name="Setup faults"
                  fill="var(--chart-c)"
                />
                <Bar
                  dataKey="timed"
                  name="Timed faults"
                  fill="var(--chart-b)"
                />
              </BarChart>
            </ResponsiveContainer>
          </div>
        </Panel>
      </div>
    </>
  );
}


function IntegrityPage({ data, onRun, running }) {
  return (
    <>
      <PageTitle
        eyebrow="CORRECTNESS"
        title="Data Integrity Verification"
        description="Validate byte counts and deterministic checksums independently of performance tests."
      />

      <ExperimentForm
        title="Verify an IPC method"
        description="Transfers deterministic data and compares sender/receiver checksums."
        command="verify"
        running={running}
        onRun={onRun}
        fields={[
          {
            key: "method",
            label: "Method",
            type: "select",
            defaultValue: "shm-opt",
            options: [
              { value: "pipe", label: "PIPE" },
              { value: "fifo", label: "FIFO" },
              { value: "socket", label: "SOCKET" },
              { value: "shm", label: "SHM" },
              { value: "shm-opt", label: "SHM-RING" },
            ],
          },
          {
            key: "payload",
            label: "Payload (MB)",
            defaultValue: "10",
          },
          {
            key: "chunk",
            label: "Chunk (KB)",
            defaultValue: "64",
          },
        ]}
      />

      <Panel
        title="Recorded Verification Evidence"
        subtitle="Checksums must match exactly"
      >
        <div className="data-table-wrap">
          <table className="data-table">
            <thead>
              <tr>
                <th>Method</th>
                <th>Payload</th>
                <th>Chunk</th>
                <th>Bytes</th>
                <th>Checksum</th>
                <th>Result</th>
              </tr>
            </thead>

            <tbody>
              {(data.integrity || []).map((row, index) => (
                <tr key={`${row._file}-${index}`}>
                  <td>{row.method}</td>
                  <td>{row.payload_mb} MB</td>
                  <td>{row.chunk_kb} KB</td>
                  <td>{Number(row.bytes_received).toLocaleString()}</td>
                  <td className="mono">{row.receiver_checksum}</td>
                  <td>
                    <Badge
                      type={
                        row.result === "PASS"
                          ? "success"
                          : "danger"
                      }
                    >
                      {row.result}
                    </Badge>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </Panel>
    </>
  );
}


function EvidencePage({ data }) {
  const env = data.environment || {};

  return (
    <>
      <PageTitle
        eyebrow="REPRODUCIBILITY"
        title="Experiment Evidence"
        description="Environment, Git revision and run-manifest evidence for the measured results."
      />

      <div className="environment-grid">
        <EnvironmentCard
          icon={Server}
          label="Operating System"
          value={env.operating_system}
        />

        <EnvironmentCard
          icon={Cpu}
          label="Processor"
          value={env.cpu_model}
        />

        <EnvironmentCard
          icon={Layers3}
          label="Kernel"
          value={env.kernel_release}
        />

        <EnvironmentCard
          icon={Box}
          label="Logical CPUs"
          value={env.logical_cpus}
        />

        <EnvironmentCard
          icon={HardDrive}
          label="Page Size"
          value={
            env.page_size_bytes
              ? `${env.page_size_bytes} bytes`
              : "—"
          }
        />

        <EnvironmentCard
          icon={Braces}
          label="Compiler"
          value={
            env.compiler
              ? `${env.compiler} ${env.compiler_version}`
              : "—"
          }
        />
      </div>

      <Panel
        title="Run Manifests"
        subtitle="Latest authoritative and supporting experiment records"
      >
        <div className="manifest-list">
          {(data.manifests || []).map((manifest) => (
            <div className="manifest-row" key={manifest.file}>
              <div className="manifest-state">
                <StatusDot
                  online={
                    manifest.status === "SUCCESS" &&
                    manifest.source_state === "CLEAN"
                  }
                />
              </div>

              <div className="manifest-main">
                <strong>
                  {manifest.category || "Experiment"}
                </strong>

                <span className="mono">
                  {manifest.command || manifest.file}
                </span>
              </div>

              <div className="manifest-meta">
                <Badge
                  type={
                    manifest.status === "SUCCESS"
                      ? "success"
                      : "danger"
                  }
                >
                  {manifest.status || "UNKNOWN"}
                </Badge>

                <span>
                  {manifest.source_state || "—"}
                </span>

                <code>
                  {manifest.commit
                    ? manifest.commit.slice(0, 8)
                    : "—"}
                </code>
              </div>
            </div>
          ))}
        </div>
      </Panel>
    </>
  );
}


function EnvironmentCard({ icon: Icon, label, value }) {
  return (
    <div className="environment-card">
      <Icon size={20} />

      <div>
        <span>{label}</span>
        <strong>{value || "—"}</strong>
      </div>
    </div>
  );
}


function PageTitle({ eyebrow, title, description }) {
  return (
    <div className="page-title">
      <span>{eyebrow}</span>
      <h1>{title}</h1>
      <p>{description}</p>
    </div>
  );
}


function App() {
  const [page, setPage] = useState("dashboard");
  const [sidebarOpen, setSidebarOpen] = useState(false);

  const [health, setHealth] = useState(null);
  const [data, setData] = useState({
    benchmark: [],
    shm_optimization: [],
    syscalls: [],
    affinity: [],
    memory: [],
    workloads: [],
    ring_slots: [],
    integrity: [],
    environment: {},
    manifests: [],
    final_summary: [],
  });

  const [loading, setLoading] = useState(true);
  const [running, setRunning] = useState(false);
  const [consoleData, setConsoleData] = useState(null);


  async function loadData() {
    try {
      const [healthResponse, dashboardResponse] =
        await Promise.all([
          fetch(`${API}/health`),
          fetch(`${API}/dashboard`),
        ]);

      const healthJson = await healthResponse.json();
      const dashboardJson = await dashboardResponse.json();

      setHealth(healthJson);
      setData(dashboardJson);
    }

    catch (error) {
      setHealth({
        status: "offline",
        fastipc_exists: false,
        error: error.message,
      });
    }

    finally {
      setLoading(false);
    }
  }


  useEffect(() => {
    loadData();
  }, []);


  async function runExperiment(command, args) {
    setRunning(true);

    setConsoleData({
      command: `./fastipc ${command} ${args.join(" ")}`.trim(),
      stdout: "",
      stderr: "",
      exit_code: null,
    });

    try {
      const response = await fetch(`${API}/run`, {
        method: "POST",

        headers: {
          "Content-Type": "application/json",
        },

        body: JSON.stringify({
          command,
          args,
        }),
      });

      const result = await response.json();

      if (!response.ok) {
        setConsoleData({
          command: `./fastipc ${command} ${args.join(" ")}`.trim(),
          stdout: "",
          stderr: result.error || "Experiment failed.",
          exit_code: 1,
        });
      }

      else {
        setConsoleData(result);

        if (result.exit_code === 0) {
          await loadData();
        }
      }
    }

    catch (error) {
      setConsoleData({
        command: `./fastipc ${command} ${args.join(" ")}`.trim(),
        stdout: "",
        stderr: error.message,
        exit_code: 1,
      });
    }

    finally {
      setRunning(false);
    }
  }


  const activeLabel = useMemo(
    () => NAV_ITEMS.find((item) => item.id === page)?.label,
    [page]
  );


  function renderPage() {
    switch (page) {
      case "benchmarks":
        return (
          <BenchmarksPage
            data={data}
            onRun={runExperiment}
            running={running}
          />
        );

      case "optimization":
        return (
          <OptimizationPage
            data={data}
            onRun={runExperiment}
            running={running}
          />
        );

      case "adaptive":
        return (
          <AdaptivePage
            data={data}
            onRun={runExperiment}
            running={running}
          />
        );

      case "syscalls":
        return <SyscallsPage data={data} />;

      case "scheduler":
        return (
          <SchedulerPage
            data={data}
            onRun={runExperiment}
            running={running}
          />
        );

      case "memory":
        return (
          <MemoryPage
            data={data}
            onRun={runExperiment}
            running={running}
          />
        );

      case "integrity":
        return (
          <IntegrityPage
            data={data}
            onRun={runExperiment}
            running={running}
          />
        );

      case "evidence":
        return <EvidencePage data={data} />;

      default:
        return <DashboardPage data={data} />;
    }
  }


  if (loading) {
    return (
      <div className="loading-screen">
        <div className="loading-logo">
          <Zap size={27} />
        </div>

        <strong>FASTIPC-X</strong>
        <span>Loading native experiment evidence...</span>
      </div>
    );
  }


  return (
    <div className="app-shell">
      <aside
        className={`sidebar ${
          sidebarOpen ? "mobile-open" : ""
        }`}
      >
        <div className="brand">
          <div className="brand-mark">
            <Zap size={21} fill="currentColor" />
          </div>

          <div>
            <strong>FASTIPC-X</strong>
            <span>Systems Lab</span>
          </div>
        </div>

        <nav>
          <span className="nav-section-label">WORKSPACE</span>

          {NAV_ITEMS.map((item) => {
            const Icon = item.icon;

            return (
              <button
                key={item.id}
                className={
                  page === item.id ? "active" : ""
                }
                onClick={() => {
                  setPage(item.id);
                  setSidebarOpen(false);
                }}
              >
                <Icon size={18} />
                <span>{item.label}</span>
                {page === item.id && (
                  <ChevronRight
                    size={16}
                    className="nav-chevron"
                  />
                )}
              </button>
            );
          })}
        </nav>

        <div className="sidebar-system">
          <div className="system-row">
            <StatusDot
              online={
                health?.status === "ready" &&
                health?.fastipc_exists
              }
            />

            <div>
              <strong>
                {health?.status === "ready"
                  ? "Engine Ready"
                  : "Engine Offline"}
              </strong>

              <span>
                {health?.fastipc_exists
                  ? "Native binary detected"
                  : "fastipc not found"}
              </span>
            </div>
          </div>

          <code>127.0.0.1:5050</code>
        </div>
      </aside>

      <main className="main-area">
        <header className="topbar">
          <div className="topbar-left">
            <button
              className="mobile-menu"
              onClick={() => setSidebarOpen(true)}
            >
              <Menu size={20} />
            </button>

            <div>
              <span>FastIPC-X</span>
              <strong>{activeLabel}</strong>
            </div>
          </div>

          <div className="topbar-right">
            <button
              className="refresh-button"
              onClick={loadData}
            >
              <RefreshCw size={16} />
              Refresh evidence
            </button>

            <div className="engine-pill">
              <StatusDot
                online={
                  health?.status === "ready" &&
                  health?.fastipc_exists
                }
              />
              Native Engine
            </div>
          </div>
        </header>

        <div className="page-content">
          {renderPage()}
        </div>
      </main>

      <ConsoleDrawer
        data={consoleData}
        running={running}
        onClose={() => setConsoleData(null)}
      />

      {sidebarOpen && (
        <button
          className="sidebar-overlay"
          onClick={() => setSidebarOpen(false)}
        />
      )}
    </div>
  );
}


export default App;
