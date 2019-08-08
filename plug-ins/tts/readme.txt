============================================================

tts : Text-To-Speech
The tts peripheral provices a simple interface to the CMU
Flite text-to-speech program.  While the audio quality is
not as good as commercial TTS systems, Flite is entirely
open source and can usually be installed with an apt-get
command.


INSTALLATION
Install Flite with the apt-get command:
   sudo apt-get install flite
See what voices are available with the command:
   flite -lv
Test that flite works with your audio system:
   flite -voice awb -t 'hello world'


RESOURCES
The tts peripheral has resources that let you set the
voice, output speech, and to monitor whether or not the
system is in use.

speak : A write-only resource that invokes the flite
command to speak the specified text on you audio system.

voice : A read-write resource that lets you specify which
voice to use for output.  Most flite installations include
the following:
   kal : The voice of a 1950's robot
   awb : Easy to understand, almost British
   slt : Easy to understand, female

status : A broadcast resource that gives the state of flite
as 'busy' or 'idle'.  You can use the get command to read
the state or the cat command to get updates as they occur.
The status resource is useful if you want to output longer
strings or if you want a longer pause between parts of your
output.


EXAMPLES
Watch the state transitions as you speak a phrase using the
slt voice.
   edcat tts status &
   edset tts voice awb
   edset tts speak To be, or not to be.  That is the question.

