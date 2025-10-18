current summary:
server uses select, when it gets a complete message it packages the message as a Job and puts in the queue. 
The worker thread sees that the queue is not empty, handles commands, puts in broadcast queue.
some stuff may need fixing?
some TODO stuff should b looked at in case of error

next steps:
handling broadcast queue
i dont think i did the queue handling correctly - use condition variable
testing/error fixing