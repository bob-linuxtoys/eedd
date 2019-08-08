============================================================

hellodemo Plug-in
The hellodemo plug-in provides a simple model for writing
new plug-ins.  The plug-in has two user configurable
resources and one broadcast resource.
   This demo plug-in lacks opening an input device for
sensor updates, and so it lacks a call to add_fd().


RESOURCES
The hellodemo plug-in prints to the broadcast resource
'message' the string contained in 'messagetext' every
'period' seconds.

messagetext : A read-write resource that has the text to
send.  The text is limited to MX_MSGLEN (60) characters
and strings longer than this are quietly truncated.

period : A read-write resource that sets the period in
seconds between updates to 'message'.  Any integer
value greater than 0 is accepted.

message : A broadcast resource that outputs 'messagetext'
every 'period' seconds.

