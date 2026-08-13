#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/_removed/PID.h"

// --- unused agg/mid gains + commented presets ---
// Legacy tuning presets (kept for reference).
// double aggKp=0.01, aggKi=0.06, aggKd=0.0012;
// double midKp=0.008, midKi=0.05, midKd=0.0009;
// double consKp=0.006, consKi=0.04, consKd=0.0007;

// Base scaling factor for the active PID gains.
double PIDKMultiplier = 2;
// Aggressive, medium, and conservative PID gains (currently not dynamically switched).
double aggKp = 0.0008 * PIDKMultiplier, aggKi = 0.005 * PIDKMultiplier, aggKd = 0.000006 * PIDKMultiplier;
double midKp = 0.0007 * PIDKMultiplier, midKi = 0.004 * PIDKMultiplier, midKd = 0.000005 * PIDKMultiplier;
double consKp = 0.0006 * PIDKMultiplier, consKi = 0.003 * PIDKMultiplier, consKd = 0.000004 * PIDKMultiplier;

// --- PIDTuningMultiplier ---

// Extra multipliers used in some PID-based search routines.
double PIDTuningMultiplier = 1;
double PIDTuningMultiplierKi = 1;

// --- PIDComputeTimer ---

// Timestamp of the last PID computation, used to enforce sampleTime.
unsigned long PIDComputeTimer;
