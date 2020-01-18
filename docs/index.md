_Siguza, 18. Jan 2020_

# cuck00

Twenty-twenty, bugs aplenty!

### Introduction

I admit I didn't expect to do _another_ blog post this month, but four days ago a bug was killed. A good one. A kernel info leak present on all Apple platforms, reachable from almost any context. At least a dozen people found this independently, and probably one or two dozen more knew about it second-hand. I have it on good authority that this tweet was related to it:

![Screenshot of qwerty's tweet][img1]

The same had happened to me about half a year earlier, and I imagine this is how it went for most folks who found it. And I have reason to believe that the final person to do so now was [Brandon Azad][bazad] of Google Project Zero, who of course reported it to Apple after allegedly ranting for a solid 5 minutes about how something like that could possibly exist. :P

From its nature of showing up where you wouldn't expect it, I dubbed the bug "cuck00".  
With that, let's jump in!

### The bug

IOKit drivers frequently use callback mechanisms, and those are usually built around mach ports. And while there is no real "standard" way in which they do that, there is a facility offered by `IOUserClient` that at least a solid number of drivers use: `OSAsyncReference64`. The type declaration for that is basically:

```cpp
typedef io_user_reference_t OSAsyncReference64[8];
```

Where `io_user_reference_t` is just a `uint64_t`, but signals that the value comes from userland.  
Drivers that want to use this facility can either create the above struct manually or call `IOUserClient::setAsyncReference64` - the implementation of that isn't even really relevant, the only relevant thing is how the message that is sent back to userland is constructed, which is done by `IOUserClient::_sendAsyncResult64` ([source][xnusrc]):

```cpp
IOReturn IOUserClient::_sendAsyncResult64(OSAsyncReference64 reference, IOReturn result, io_user_reference_t args[], UInt32 numArgs, IOOptionBits options)
{
    struct ReplyMsg
    {
        mach_msg_header_t msgHdr;
        union
        {
            struct
            {
                OSNotificationHeader notifyHdr;
                IOAsyncCompletionContent asyncContent;
                uint32_t args[kMaxAsyncArgs];
            } msg32;
            struct
            {
                OSNotificationHeader64 notifyHdr;
                IOAsyncCompletionContent asyncContent;
                io_user_reference_t args[kMaxAsyncArgs] __attribute__ ((packed));
            } msg64;
        } m;
    };
    ReplyMsg replyMsg;
    mach_port_t replyPort;
    kern_return_t kr;

    // If no reply port, do nothing.
    replyPort = (mach_port_t)(reference[0] & ~kIOUCAsync0Flags);
    if(replyPort == MACH_PORT_NULL)
        return kIOReturnSuccess;
    if(numArgs > kMaxAsyncArgs)
        return kIOReturnMessageTooLarge;
    bzero(&replyMsg, sizeof(replyMsg));
    replyMsg.msgHdr.msgh_bits        = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND /*remote*/, 0 /*local*/);
    replyMsg.msgHdr.msgh_remote_port = replyPort;
    replyMsg.msgHdr.msgh_local_port  = 0;
    replyMsg.msgHdr.msgh_id          = kOSNotificationMessageID;
    if(kIOUCAsync64Flag & reference[0])
    {
        replyMsg.msgHdr.msgh_size = sizeof(replyMsg.msgHdr) + sizeof(replyMsg.m.msg64) - (kMaxAsyncArgs - numArgs) * sizeof(io_user_reference_t);
        replyMsg.m.msg64.notifyHdr.size = sizeof(IOAsyncCompletionContent) + numArgs * sizeof(io_user_reference_t);
        replyMsg.m.msg64.notifyHdr.type = kIOAsyncCompletionNotificationType;
        bcopy(reference, replyMsg.m.msg64.notifyHdr.reference, sizeof(OSAsyncReference64));

        replyMsg.m.msg64.asyncContent.result = result;
        if(numArgs)
            bcopy(args, replyMsg.m.msg64.args, numArgs * sizeof(io_user_reference_t));
    }
    else
    {
        unsigned int idx;

        replyMsg.msgHdr.msgh_size = sizeof(replyMsg.msgHdr) + sizeof(replyMsg.m.msg32) - (kMaxAsyncArgs - numArgs) * sizeof(uint32_t);

        replyMsg.m.msg32.notifyHdr.size = sizeof(IOAsyncCompletionContent) + numArgs * sizeof(uint32_t);
        replyMsg.m.msg32.notifyHdr.type = kIOAsyncCompletionNotificationType;
        for(idx = 0; idx < kOSAsyncRefCount; idx++)
            replyMsg.m.msg32.notifyHdr.reference[idx] = REF32(reference[idx]);
        replyMsg.m.msg32.asyncContent.result = result;
        for(idx = 0; idx < numArgs; idx++)
            replyMsg.m.msg32.args[idx] = REF32(args[idx]);
    }
    if((options & kIOUserNotifyOptionCanDrop) != 0)
    {
        kr = mach_msg_send_from_kernel_with_options(&replyMsg.msgHdr, replyMsg.msgHdr.msgh_size, MACH_SEND_TIMEOUT, MACH_MSG_TIMEOUT_NONE);
    }
    else
    {
        /* Fail on full queue. */
        kr = mach_msg_send_from_kernel_proper(&replyMsg.msgHdr, replyMsg.msgHdr.msgh_size);
    }
    if((KERN_SUCCESS != kr) && (MACH_SEND_TIMED_OUT != kr) && !(kIOUCAsyncErrorLoggedFlag & reference[0]))
    {
        reference[0] |= kIOUCAsyncErrorLoggedFlag;
        IOLog("%s: mach_msg_send_from_kernel_proper(0x%x)\n", __PRETTY_FUNCTION__, kr);
    }
    return kr;
}
```

The critical bits are these two lines:

```cpp
replyPort = (mach_port_t)(reference[0] & ~kIOUCAsync0Flags);
```
```cpp
bcopy(reference, replyMsg.m.msg64.notifyHdr.reference, sizeof(OSAsyncReference64));
```

If you don't see it, maybe take a minute here before reading on. It should be rather obvious.

&nbsp;

&nbsp;

So... I don't know who came up with that.

This function takes an `OSAsyncReference64` and sends that, among other things, to userland in a mach message. Except it expects the first element of that `OSAsyncReference64` to be a `mach_port_t`, i.e. the mach port to send it to. And in the kernel, a `mach_port_t` is a raw pointer! So the _only_ way to use this function is to pass it a struct containing a kernel pointer _that it will then send to userland_! Every successful invocation of this function does it! There is no way around it!

And to add insult to injury, this bug isn't new either. It is present in [`xnu-123.5`][oldxnu], the very first version of XNU ever released on `opensource.apple.com`, with a copyright notice dating back to the year 2000.  
Twenty! Years! For a bug, that's not even old, that is god damn ancient! It's literally older than some of the people who are hacking iOS _today_! Just take a moment to appreciate that!

Now, with that rant out of the way, let's go on.

### Exploiting it

Literally all you have to do is invoke that function. You can pick any IOKit driver you have access to. I chose `IOSurface` because that's available in the contexts most people care about (3rd party app container and `WebContent`), and exists both on iOS and macOS.

After the usual IOSurface setup (creation of the userclient and surface), all you have to do is:

- Call `setNotify` (external method 17) with one of the async functions and pass it a mach port.
- Call `incrementUseCount` followed by `decrementUseCount` (methods 14 and 15 respectively) - I have no idea what they're really intended for, but if the count they operate on hits zero, a message is sent back to userland.
- Receive a message on your mach port and enjoy your free kernel pointer.

```
% ./cuck00
port: 100b, (os/kern) successful
service: 1803
client: 1707, (os/kern) successful
newSurface: 1b, (os/kern) successful
setNotify: (os/kern) successful
incrementUseCount: (os/kern) successful
decrementUseCount: (os/kern) successful
mach_msg: (os/kern) successful
port addr: 0xffffff8070c57808
```

You can find a working implementation of that for iOS 13.3 and macOS 10.15.2 [here][github].

At that point you've exhausted the power of this bug and need some sort of memory corruption, so we can look at the different possibilities here.

The pointer you get is the address of a mach port, and the nature of the bug requires it to be the pointer to the very port that you received the message on.  
So if you had a bug where you need a mach port pointer for anything, you could either use that as-is to maybe trigger a `free` too many and get a UaF, or by freeing it and reallocating the memory with a different, more privileged port (e.g. a task port).  
For any other kind of bug that you need a valid kernel pointer for, you could simply deallocate the entire page the port is on, get it out of the zone and reallocate it with arbitrary contents.

But we can take this even further. Rumour has it that Apple's next generation of iDevices is gonna have memory tagging. And given this history, it would make a lot of sense:

- A10 -> ARMv8.1 (PAN)
- A11 -> ARMv8.2
- A12 -> ARMv8.3 (PAC)
- A13 -> ARMv8.4
- _A14 -> ARMv8.5 (MTE)?_

For the uninitiated, MTE ("memory tagging extension") is a new mitigation outlined by the ARMv8.5 specification. The basic concept is to have a few bits of tagging information embedded into the top bytes of data pointers, to have the hardware validate that tag against an off-site storage on access, and to change said tag whenever the memory is allocated/freed.  
As far as mitigations go, it's probably one of the strongest concepts put forward in recent years, and would in theory kill a lot of memory corruption bugs, especially use-after-frees. And while the actual implementation will likely end up being flawed if history has taught us anything, we can try and get an edge by seeing if we can break the theory behind it already.

And this bug is pretty great in that, on an ARMv8.5 system, it would've given you not just the _address_ of a mach port, but also the _tag_ that goes with it. No mitigation in the world could've prevented a _voluntary_ copyout to userland. As far as info leaks go, that's as good as it gets!  
Of course you'd still need a separate bug that survives MTE and makes use of a tagged pointer but hey, if there's a voluntary copyout of a kernel pointer, it's at least conceivable to imagine a voluntary copyin of one, and such a bug would precisely require what this bug gives us.  
You'd be pretty constrained by the fact that you couldn't reallocate the mach port without invalidating your dat, but you could probably still make something work with the couple of fields that you can modify from userland.

### The fix

This bug was fixed in iOS 13.3.1 beta 2 released on the 14th of January 2020, CVE pending.  
Here's the relevant assembly passage from that version versus 13.3:

```
# 13.3 iPhone11,8                       # 13.3.1-b2 iPhone12,1

ldp q0, q1, [x19]                       ldur q0, [x19, 8]
ldp q2, q3, [x19, 0x20]                 ldur q1, [x19, 0x18]
stur q3, [sp, 0x68]                     ldur q2, [x19, 0x28]
stur q2, [sp, 0x58]                     ldr x8, [x19, 0x38]
stur q1, [sp, 0x48]                     str x8, [sp, 0x70]
stur q0, [sp, 0x38]                     stp q1, q2, [sp, 0x50]
                                        str q0, [sp, 0x40]
```

So literally all they did was change the `bcopy` to exclude the first element/8 bytes of `reference`.  
And with that the bug is gone, crumbled to dust.

### Conclusion

_\*laughs in disbelief\*_

For twenty years there've been kernel pointers all over userland.  
And that breaks mitigations that aren't even out yet.  
You just can't make that up.

I salute everyone who I know found this bug independently, especially those who are younger than the bug itself. ;P  
This was a good bug, may we find many more like it! :D

Again, a fully functional exploit is [available on GitHub][github].

Feel free to hit me up [on Twitter][twitter] or via mail (`*@*.net` where `*` = `siguza`) if you have feedback, questions, or just want to chat.

Cheers. :)

  [github]: https://github.com/Siguza/cuck00
  [twitter]: https://twitter.com/s1guza
  [bazad]: https://twitter.com/_bazad
  [xnusrc]: https://opensource.apple.com/source/xnu/xnu-4903.241.1/iokit/Kernel/IOUserClient.cpp.auto.html
  [oldxnu]: https://opensource.apple.com/source/xnu/xnu-123.5/
  [img1]: assets/img/1-tweet.png
