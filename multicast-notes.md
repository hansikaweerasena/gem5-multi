
# Multicast Development Notes

## 2023-04-02 -- se.py has been deprecated

I switched to the `develop` branch (as recommended by the gem5 contribution docs),
compiled it, and tried running a test simulation using se.py.
I got the following error:
```
fatal: The 'configs/example/se.py' script has been deprecated.
It can be found in 'configs/deprecated/example' if required.
Its usage should be avoided as it will be removed in future releases of gem5.
```
I need to know why se.py was deprecated and what the recommended alternative is.

Using git-blame and git-show, I found the commit where this was changed.
Here is a link to the code review and discussion:
https://gem5-review.googlesource.com/c/public/gem5/+/68157 .

There is not much info here, but the conversation indicates
the gem5 developers really want people to stop using se.py and fs.py.

More information:
https://gem5.atlassian.net/browse/GEM5-1278

> This whole process is a big thumbs-up from me.
> These scripts are causing us a lot of headaches and have become difficult to maintain.
> The gem5 stdlib should be able to meet the vast majority of se/fs.py use-cases come the next release.

What is the "gem5 stdlib"?
