
## 14:30: the App cryptex binder reports ENOENT; path diagnostics added

`load_init_program` now binds the App cryptex right after the SystemOS mount and reports the result:

    offline cryptex: App bind=2 SpringBoard=2      (2 = ENOENT)

so the bind fails because one of the two paths is absent.  The next image prints each lookup separately:

    offline cryptex: src(/private/preboot/Cryptexes/App)=? dst(/System/Cryptexes/App)=?

which decides whether the App tree is not where the SystemOS mount puts it, or the mount point is missing from
the composed root.

Also established this round: the official `patch-reference` names `offline-owner-systemos-mount` as the step that
mounts SystemOS at `/private/preboot` and composes the root, which is why the App bind belongs immediately after it
rather than in the GPU-addon cryptex view.