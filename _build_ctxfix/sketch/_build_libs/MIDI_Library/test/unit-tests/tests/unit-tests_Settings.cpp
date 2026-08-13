#line 1 "/home/felipe/Documentos/DCO3-MONOSYNTH/DCO/_build_libs/MIDI_Library/test/unit-tests/tests/unit-tests_Settings.cpp"
#include "unit-tests_Settings.h"

BEGIN_MIDI_NAMESPACE

const bool DefaultSettings::UseRunningStatus;
const bool DefaultSettings::HandleNullVelocityNoteOnAsNoteOff;
const bool DefaultSettings::Use1ByteParsing;
const unsigned DefaultSettings::SysExMaxSize;

END_MIDI_NAMESPACE

// -----------------------------------------------------------------------------

BEGIN_UNNAMED_NAMESPACE

TEST(Settings, hasTheRightDefaultValues)
{
    EXPECT_EQ(midi::DefaultSettings::UseRunningStatus,                   false);
    EXPECT_EQ(midi::DefaultSettings::HandleNullVelocityNoteOnAsNoteOff,  true);
    EXPECT_EQ(midi::DefaultSettings::Use1ByteParsing,                    true);
    EXPECT_EQ(midi::DefaultSettings::SysExMaxSize,                       unsigned(128));
}

END_UNNAMED_NAMESPACE
