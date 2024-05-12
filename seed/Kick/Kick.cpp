#include "daisy_seed.h"
#include "daisysp.h"

// Use the daisy namespace to prevent having to type
// daisy:: before all libdaisy functions
using namespace daisy;
using namespace daisysp;

// Declare a DaisySeed object called hardware
DaisySeed hardware;
const int AUDIO_BLOCK_SIZE = 1;

// Polyphony
const int VOICE_COUNT   = 8;
int       current_voice = 0;


// DSP
Metro      clock;
Oscillator osc[VOICE_COUNT];
AdEnv      env[VOICE_COUNT];
AdEnv      pitch_env[VOICE_COUNT];
Svf        hp;
Svf        mx_h[3];
Svf        mx_m[3];
Svf        mx_l[3];

// Mixer
const int MIXER_CHANNEL = 1;
int       current_mx    = 0;
int       mx_h_amp_val[MIXER_CHANNEL];
int       mx_m_freq_val[MIXER_CHANNEL];
int       mx_m_amp_val[MIXER_CHANNEL];
int       mx_l_amp_val[MIXER_CHANNEL];
int       mx_gain_val[MIXER_CHANNEL];

// Controls
AnalogControl controls[16];
Switch        button1, button2, button3;
const int     UPDATE_RATE = 1;
int           update_step = 0;

// Parameters
Parameter clock_speed, decay, osc_freq, osc_amp, amp_curve, pitch_decay,
    pitch_curve, pitch_mod_depth, mx_h_amp, mx_m_freq, mx_m_amp, mx_l_amp,
    mx_gain;

// Logging
const bool LOGGING  = false;
const int  LOG_RATE = 48000 / 8 / AUDIO_BLOCK_SIZE;
int        log_step = 0;

void UpdateDigitalControls();
void UpdateParameters();
void UpdateMixerParameters();
void AdvanceSequencer();
void SetEnvelopeParameters();
void NextSamples(float &sig);
void Mixer(float &sig);
void OutputLog();

void AudioCallback(AudioHandle::InterleavingInputBuffer  in,
                   AudioHandle::InterleavingOutputBuffer out,
                   size_t                                size)
{
    float sig;

    if(LOGGING && log_step == 0)
    {
        OutputLog();
    }
    log_step++;
    log_step %= LOG_RATE;

    if(update_step == 0)
    {
        // this for some reason needs to be inside the if statement to work
        UpdateParameters();
        UpdateMixerParameters();
        UpdateDigitalControls();
    }
    update_step++;
    update_step %= UPDATE_RATE;

    if(button2.RisingEdge())
    {
        current_mx++;
        current_mx %= MIXER_CHANNEL;
    }

    if(current_mx > 0 && button3.RisingEdge())
    {
        current_mx--;
    }

    AdvanceSequencer();
    SetEnvelopeParameters();

    clock.SetFreq(clock_speed.Value());

    sig = 0;
    for(size_t i = 0; i < size; i += 2)
    {
        NextSamples(sig);
        Mixer(sig);

        hp.Process(sig);
        sig = hp.High();

        out[i]     = sig;
        out[i + 1] = sig;
    }
}


int main(void)
{
    // Configure and Initialize the Daisy Seed
    // These are separate to allow reconfiguration of any of the internal
    // components before initialization.
    hardware.Configure();
    hardware.Init();
    hardware.SetAudioBlockSize(AUDIO_BLOCK_SIZE);
    if(LOGGING)
    {
        hardware.StartLog(true);
    }

    //How many samples we'll output per second
    float samplerate = hardware.AudioSampleRate();

    clock.Init(60 / 60, samplerate);

    //Create an ADC configuration
    AdcChannelConfig adcConfig[2];
    adcConfig[0].InitMux(seed::A4, 8, seed::D20, seed::D21, seed::D22);
    adcConfig[1].InitMux(seed::A11, 8, seed::D12, seed::D13, seed::D14);

    //Initialize the buttons
    button1.Init(seed::D25);
    button2.Init(seed::D24);
    button3.Init(seed::D23);

    //Set the ADC to use our configuration
    hardware.adc.Init(adcConfig, 2);

    //Initialize the analog controls
    int channel_idx = 0;
    for(int i = 0; i < 16; i++)
    {
        if(i != 0 && i % 8 == 0)
        {
            channel_idx++;
        }
        controls[i].Init(hardware.adc.GetMuxPtr(channel_idx, i % 8),
                         samplerate / UPDATE_RATE,
                         false,
                         false,
                         0.1);
    }


    //Initialize the parameters
    clock_speed.Init(controls[8], 0.2, 10, Parameter::LINEAR);
    decay.Init(controls[9], 0, 4, Parameter::LINEAR);
    osc_freq.Init(controls[10], 1, 64, Parameter::EXPONENTIAL);
    osc_amp.Init(controls[11], 0, 4, Parameter::LINEAR);
    amp_curve.Init(controls[12], -7, 2, Parameter::LINEAR);
    pitch_decay.Init(controls[13], 0, 8, Parameter::LINEAR);
    pitch_curve.Init(controls[14], -10, 2, Parameter::LINEAR);
    pitch_mod_depth.Init(controls[15], 0, 1000, Parameter::LINEAR);
    mx_h_amp.Init(controls[0], 0, 4, Parameter::LINEAR);
    mx_m_freq.Init(controls[1], 100, 8000, Parameter::LINEAR);
    mx_m_amp.Init(controls[2], 0, 4, Parameter::LINEAR);
    mx_l_amp.Init(controls[3], 0, 4, Parameter::LINEAR);
    mx_gain.Init(controls[4], 0, 4, Parameter::LINEAR);


    //Set up oscillators
    for(int i = 0; i < VOICE_COUNT; i++)
    {
        osc[i].Init(samplerate);
        osc[i].SetWaveform(osc[i].WAVE_SIN);
        osc[i].SetAmp(1.f);
        osc[i].SetFreq(1000);
    }

    //Set up volume envelopes
    for(int i = 0; i < VOICE_COUNT; i++)
    {
        env[i].Init(samplerate);
        env[i].SetTime(ADENV_SEG_ATTACK, .001);
        env[i].SetTime(ADENV_SEG_DECAY, .4);
        env[i].SetMin(0.0);
        env[i].SetMax(1.f);
        env[i].SetCurve(0);
    }

    //Set up pitch envelopes
    for(int i = 0; i < VOICE_COUNT; i++)
    {
        pitch_env[i].Init(samplerate);
        pitch_env[i].SetTime(ADENV_SEG_ATTACK, .001);
        pitch_env[i].SetTime(ADENV_SEG_DECAY, .4);
        pitch_env[i].SetMin(0.0);
        pitch_env[i].SetMax(1000.f);
        pitch_env[i].SetCurve(0);
    }

    //Set up filters
    for(int i = 0; i < 3; i++)
    {
        mx_h[i].Init(samplerate);
        mx_h[i].SetFreq(12000);
        mx_h[i].SetRes(0.2);

        mx_m[i].Init(samplerate);
        mx_m[i].SetFreq(1000);
        mx_m[i].SetRes(0.2);

        mx_l[i].Init(samplerate);
        mx_l[i].SetFreq(80);
        mx_l[i].SetRes(0.2);
    }

    //Set up Mixer parameters
    for(int i = 0; i < MIXER_CHANNEL; i++)
    {
        mx_h_amp_val[i]  = 1;
        mx_m_freq_val[i] = 1000;
        mx_m_amp_val[i]  = 1;
        mx_l_amp_val[i]  = 1;
        mx_gain_val[i]   = 1;
    }

    hp.Init(samplerate);
    hp.SetFreq(10);
    hp.SetRes(0.1);

    //Start the adc
    hardware.adc.Start();

    //Start calling the audio callback
    hardware.StartAudio(AudioCallback);

    // Loop forever
    for(;;) {}
}

void UpdateDigitalControls()
{
    // debounce Buttons
    button1.Debounce();
    button2.Debounce();
    button3.Debounce();
}

void UpdateParameters()
{
    clock_speed.Process();
    decay.Process();
    osc_freq.Process();
    osc_amp.Process();
    amp_curve.Process();
    pitch_decay.Process();
    pitch_curve.Process();
    pitch_mod_depth.Process();
    mx_h_amp.Process();
    mx_m_freq.Process();
    mx_m_amp.Process();
    mx_l_amp.Process();
    mx_gain.Process();
}

void SetEnvelopeParameters()
{
    for(int i = 0; i < VOICE_COUNT; i++)
    {
        env[i].SetTime(ADENV_SEG_DECAY, decay.Value());
        env[i].SetCurve(amp_curve.Value());

        pitch_env[i].SetTime(ADENV_SEG_DECAY, pitch_decay.Value());
        pitch_env[i].SetCurve(pitch_curve.Value());
        pitch_env[i].SetMax(pitch_mod_depth.Value());
    }
}

void AdvanceSequencer()
{
    if(clock.Process())
    {
        osc[current_voice].Reset();
        env[current_voice].Trigger();
        pitch_env[current_voice].Trigger();
        current_voice++;
        current_voice %= VOICE_COUNT;
    }
}

void NextSamples(float &sig)
{
    for(int i = 0; i < VOICE_COUNT; i++)
    {
        osc[i].SetFreq(mtof(osc_freq.Value()) + pitch_env[i].Process());
        osc[i].SetAmp(env[i].Process() * osc_amp.Value());
        sig += osc[i].Process();
    }
}

void Mixer(float &sig)
{
    for(int i = 0; i < MIXER_CHANNEL; i++)
    {
        sig *= mx_gain_val[i];

        mx_h[i].Process(sig);
        float h = mx_h[i].Band();
        mx_m[i].SetFreq(mx_m_freq_val[i]);
        mx_m[i].Process(sig);
        float m = mx_m[i].Band();
        mx_l[i].Process(sig);
        float l = mx_l[i].Band();

        sig = (mx_h_amp_val[i] * h) + (mx_m_amp_val[i] * m)
              + (mx_l_amp_val[i] * l);

        sig = tanh(sig);
    }
}

void UpdateMixerParameters()
{
    mx_h_amp_val[current_mx]  = mx_h_amp.Value();
    mx_m_freq_val[current_mx] = mx_m_freq.Value();
    mx_m_amp_val[current_mx]  = mx_m_amp.Value();
    mx_l_amp_val[current_mx]  = mx_l_amp.Value();
    mx_gain_val[current_mx]   = mx_gain.Value();
}

void OutputLog()
{
    // hardware.PrintLine("Clock speed: %f", clock_speed.Value());
    // hardware.PrintLine("Decay: %f", decay.Value());
    // hardware.PrintLine("Osc freq: %f", osc_freq.Value());
    // hardware.PrintLine("Osc amp: %f", osc_amp.Value());
    // hardware.PrintLine("Amp curve: %f", amp_curve.Value());
    // hardware.PrintLine("Pitch decay: %f", pitch_decay.Value());
    // hardware.PrintLine("Pitch curve: %f", pitch_curve.Value());
    // hardware.PrintLine("Pitch mod depth: %f", pitch_mod_depth.Value());
    hardware.PrintLine("Current mx: %d", current_mx);
    // hardware.PrintLine("Mx h amp: %f", mx_h_amp.Value());
    // hardware.PrintLine("Mx m freq: %f", mx_m_freq.Value());
    // hardware.PrintLine("Mx m amp: %f", mx_m_amp.Value());
    // hardware.PrintLine("Mx l amp: %f", mx_l_amp.Value());
    // hardware.PrintLine("Mx gain: %f", mx_gain.Value());
}