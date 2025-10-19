current summary:
server uses select, when it gets a complete message it packages the message as a Job and puts in the queue. 
The worker thread sees that the queue is not empty, handles commands, puts in broadcast queue.
some stuff may need fixing?
some TODO stuff should b looked at in case of error
edited to remove unp instances and wrapper functions
adding parts of missing code frrom lab3
fixing segfault in initial thread creation

next steps:
handling broadcast queue
i dont think i did the queue handling correctly - use condition variable
testing/error fixing
handle client connections (farrah)