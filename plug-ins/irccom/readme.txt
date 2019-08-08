OVERVIEW
   The IRC peripheral provides a lightweight, easy to use, way
for robots to communicate with each other.  Each robot is assigned
a unique name, an IRC server name (or address), and two 'channels'
over which it can communicate.  Channels are usually set up for
different groups or topics.  For example, a RoboSoccer game might
have a channel for the red team, a channel for the blue team, and
a channel for the referees.
   This system does not use SSL or passwords.  DO NOT use this
system where security matters.


IRC SERVER
   Each robot must have access to an IRC server.  The server can
be on the Internet but is most often on a Raspberry Pi (or SBC)
connected to a WiFi router via Ethernet.  If this description
matches your system you can install an IRC server on the Pi
with:
  sudo apt-get install ngircd
You can add permanent channels to your IRC server by appending
the following to the /etc/ngircd/ngircd.conf:
  [Channel]
          Name = &referee
          Topic = Referee's channel
  [Channel]
          Name = &blueteam
          Topic = Blue team channel
  [Channel]
          Name = &redteam
          Topic = Red team channel
While editing the configuration file you should also change the
message-of-the-day and the server name.  If you change ngircd's
configuration file be sure to restart it with:
  sudo pkill -1 ngircd



RESOURCES
   The user interfaces to the IRC peripheral let you specify
which IRC server to use, your robot's name, and which two channels
to use.  There are also resources that let you get a list of the
channels available at the server and to give the status of the
connection to the IRC server.

config
   The the name for this robot followed by the domain name or IP
address of the IRC server to use.  The robot name must be unique
on the server, must be nine characters or less, and should not
contain embedded white space or commas.  R2D2, Robbie, Bender,
and Gort are all good robot names.  Writing to this node initiates
a connection to the IRC server with the specified name.  This
resource works with the get and set commands.  There are no
background timers so you MUST write to this resource to establish
initial communication to the server.  There is a retry timer
in case the connection is lost.

status
   The status of the connection to an IRC server.  Values for
status can be 'NoServer', 'Connecting', 'Error', and 'Connected'.
NoServer indicates that no IRC server has been specified in config.
Connecting indicates that the IRC peripheral is currently trying
to establish a connection to the IRC server.  An Error status is
given if there was a timeout while trying to set up a connection,
if an existing connection is interrupted, or if the requested name
is already in use on the IRC server.  A Connected state indicates
that the IRC connection is established.  This resource works with
the get command.

available_channels
   Once a connection to the IRC server is established, you can
use this resource to get a list of the channels available on the
server.  There is one channel per line, and each line contains the
channel name, a tab, and a brief description of the channel.
This resources works with the get command.  Note that the '&'
that is prepended to the actual IRC channel names is removed from
the list of channels.   This resource works with the get command.

my_channels
   A space separate list of two channels to use.  Channels do not
have the usual '&' prepended to them.  This makes this resource
easier to use in shell scripts.  This resource works with the
get and set commands.

comm
   Data to and from the robot.  Use the set command to write
messages and the cat command to receive the stream of messages.
The form of the set command to send a message is:
   edset irccom comm <channel_name> [text to send]
For example, send the word READY to red team channel:
   edset irccom comm g1_red READY
The only way to receive messages is with the cat command.  The
messages in the data steam are of the form:
   <channel_name> <sender's_name> [text of the message]



EXAMPLE
   Let's continue with the example of a RoboSoccer game.  Team members
can communicate with each other and all participants hear the referees.
Bash configuration for a member of the red team might appear as:

     # connect to the local IRC server as Red6
     edset irccom config Red6 192.168.1.20
     # verify we are connected
     edget irccom status
     # get a list of the available channels
     edget irccom available_channels
     # set our two channels to referee and redteam
     edset irccom my_channels referee redteam
     # start listening for referee commands
     edcat irccom comm &
     # tell the red captain we're ready to go
     edset irccom comm redteam ready


