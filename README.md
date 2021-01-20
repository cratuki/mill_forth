# Status

This project builds but has little functionality. It discredited its
original hypothesis.

The starting hypothesis,

    That a gas-powered forth engine would be useful as a general-purpose
    interpreter to be embedded in larger systems.

The 'gas' concept serves the same role as Ethereum gas,

    The enclosing system periodically gives an amount of 'gas' to our
    interpreter that is consumed as it runs.
    
    In this respect, the machine is technically not a turing machine, and
    cannot get into infinite loops.

Through work, I formed a conclusion that there are better approaches to create
a gas-powered interpreter. For example, a better approach would be to create a
gas-powered virtual machine (either stack or register-based) and them to
implement forth on top of that.

There was a further design error in the implementation of the stack here,

    Implementation of the stack is done via pooling of malloc'd blocks.
    
    The design would be tighter if the stack used the same memory space as the
    dictionary. Each could grow from different ends. Other forth systems use
    this approach.


# License (MIT)

Copyright (c) 2020, Craig Turner

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

