"""A small project in the MPC Sample's format (gzip: five header lines, then JSON), shaped like a real one: the input,
two outputs, a submix, a drum track and the submix/output programs that mirror the mixer. Also a settings file."""
import gzip, json, os, sys

def fx(i, name, fmt="MPC", on=True):
    f = os.environ.get("FIXTURE_FILE_" + name.replace(" ", "_"), name)   # a real library for the live test
    return {"index": i, "plugin": {"version": 2, "is64Bits": True, "description": {"version": 1, "name": name,
            "descriptiveName": name, "pluginFormatName": fmt, "category": "", "manufacturerName": "x",
            "fileOrIdentifier": f, "uid": 1}, "state": "", "programNumber": 0}, "enable": on}

def mix(*effects):
    return {"volume": 1.0, "inserts": {"insertsEnabled": True, "effects": list(effects)}}

d = {"data": {"mixer": {
        "input": {"name": "in", "mixable": mix(fx(0, "LP Filter"), fx(1, "NAM", "VST"))},
        "outputs": [{"mixable": mix(fx(0, "Pad-Controlled FXs"), fx(1, "Flex Beat Player"), fx(2, "Color Compressor"))},
                    {"mixable": mix()}],
        "submixes": [{"mixable": mix(fx(0, "LP Filter"))}]},
    "tracks": [{"name": "Drum 001", "program": {"type": 0, "mixable": mix(), "drum": {"instruments": [{"mixable": mix()}]}}},
               {"name": "Submix 1", "program": {"type": 8, "mixable": mix(fx(0, "LP Filter"))}},
               {"name": "Out 1/2", "program": {"type": 9, "mixable": mix()}}]}}
out = sys.argv[1]
gzip.open(out, "wb").write(b"ACVS\n1.3.0.12\nSerialisableProjectData\njson\nLinux\n" + json.dumps(d, indent=1).encode())
open(out.rsplit("/", 1)[0] + "/original_project_data.txt", "w").write("Riff Ideas\n/media/MPCSD/Riff Ideas\n")
open(sys.argv[2], "w").write('''<?xml version="1.0"?>
<PROPERTIES>
  <VALUE name="pluginList-arm-64bit">
    <KNOWNPLUGINS>
      <PLUGIN name="Tuner" descriptiveName="Tuner" format="VST" category="Effect"
              manufacturer="jacob-sabella" version="1.0" file="/media/MPCSD/Synths/a - VST - Tuner/tuner.so"
              uid="54756e72" isInstrument="0" numInputs="2" numOutputs="2" isShell="0"/>
      <PLUGIN name="Synth &amp; Co" format="VST" category="Synth" manufacturer="m" file="/s.so" uid="1" isInstrument="1"/>
      <PLUGIN name="Gain &quot;PoC&quot;" format="VST" category="Effect" manufacturer="j &amp; s" file="/g g/gain.so" uid="46476e50" isInstrument="0"/>
    </KNOWNPLUGINS>
  </VALUE>
  <VALUE name="pluginList-arm">
    <KNOWNPLUGINS><PLUGIN name="Other" format="VST" file="/o.so" uid="2" isInstrument="0"/></KNOWNPLUGINS>
  </VALUE>
</PROPERTIES>
''')
