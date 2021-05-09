# NoiceCancel
Trying to cancel noise on a Window system using software

This is a simple application to try and cancel noise using the default mic and speakers on a Windows laptop. This uses the waveIn and waveOut APIs of Windows to capture
the background noise and use a simple LMS filter to play the anti noise using the speakers to cancel out the noise. Sounds good in theory!

However the latencies involved might make this a challenging task. 
