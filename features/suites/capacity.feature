Feature: capacity

  @VR-01 @build
  Scenario: advance a verified checkpoint within budget
    Given the captured page bytes agree and the measured charge fits
    When the checkpoint is below the ceiling
    Then the next operation is advance

  @VR-02 @build @rejects
  Scenario: refuse a corrupt readback
    Given the restored bytes differ from the input
    When a checkpoint is submitted
    Then the model answers failed readback

  @VR-03 @open
  Scenario: measure capacity across content distributions
    Given a configured host budget and identified backend
    When the guest or host exercises ordinary memory
    Then report checked bytes, backing charge and elapsed time

  @VR-04 @build
  Scenario: Choose a measured search configuration
    Given an incumbent and a byte-verified candidate
    When the candidate uses fewer backing bytes
    Then the controller replaces the incumbent

  @VR-05 @build @rejects
  Scenario: Reject an empty controller input
    Given an empty request
    When the controller dispatches it
    Then the response is "invalid request"

  @VR-06 @build
  Scenario: Schedule a bounded extent and assess its observations
    Given an eight MiB checkpoint and a twelve MiB ceiling
    When the model schedules the next trial
    Then the extent is twelve MiB and a partial observation stops at budget

  @VR-07 @build
  Scenario: Size a managed device and retire unused page groups
    Given 8192 MiB of available memory
    When a managed device starts and a group loses its final occupied page
    Then its automatic size is 1024 MiB and the empty group is released

  @VR-08 @open
  Scenario: Reuse an exact literal across unrelated swap addresses
    Given equal high-entropy pages and independently generated high-entropy controls
    When the device receives them under pressure
    Then measure exact restores, shared literal allocations and write time

  @VR-09 @open
  Scenario: Reconstruct a dense tensor from an immutable base and changed coordinates
    Given high-entropy pages with a small number of changed coordinates
    When the original address is overwritten or discarded
    Then every dependent page still reconstructs its original bytes

  @VR-10 @build
  Scenario: Resize the dense-base index with the live dictionary
    Given an index with 4096 buckets
    When the number of live bases increases from 16383 to 16384
    Then the next bucket count increases from 4096 to 8192

  @VR-11 @open
  Scenario: Online Verdant memory in a running operating system
    Given a Linux guest booted on a shared Verdant memory file with swap disabled
    When a second range of that store is plugged as a DIMM
    Then MemTotal increases and an ordinary allocation reads back bytes from the added physical range

  @VR-12 @open
  Scenario: Reuse provisioned storage for independent literal bases
    Given a private backing file and pages sharing a dense base
    When the original address changes and retired slots are reused
    Then surviving references restore their original bytes from the Verdant store

  @VR-13 @open
  Scenario: Fit a dense page with a single read of each candidate base
    Given several distinct file-backed pages with equal sample fingerprints
    When a page matches one base exactly or differs at a few coordinates
    Then each candidate is read at most once and the chosen representation restores every byte

  @VR-14 @open
  Scenario: Preserve a sparse address space after a backing write error
    Given one committed page and previously untouched page groups
    When writes to the backing file fail
    Then the committed page retains its bytes and the empty groups release their metadata

  @VR-15 @build
  Scenario: Select the greatest admissible capacity without a fixed device ceiling
    Given platform, guest and backing limits expressed in whole MiB
    When the capacity policy chooses the extent
    Then it satisfies all three limits and every feasible extent is at most the chosen extent

  @VR-15 @build @rejects
  Scenario: Reject a target one byte short of a full reserve
    Given platform and guest limits of 100 and 64 MiB
    When the backing budget is one byte below 64 MiB plus the replacement page
    Then a proposed ceiling of 64 MiB receives "capacity mismatch"

  @VR-16 @open
  Scenario: Touch distant addresses in a large logical device
    Given a small process budget and a sparse logical extent above one TiB
    When the first and last pages are written and the extent is discarded
    Then both restore exactly before discard and the empty directory has its starting charge

  @VR-17 @open
  Scenario: Reclaim and reuse interleaved slots in a full dense backing file
    Given a full arena ending in a partial word and live literals in alternating slots
    When the other slots retire and new literals fill their places
    Then the release uses no additional slot metadata and every surviving literal restores its bytes

  @VR-18 @open
  Scenario: Converge independent pages onto one shared literal
    Given a populated file-backed dictionary
    When its pages are overwritten with the first page's exact contents
    Then discarding an untouched page has no further effect on the store charge

  @VR-19 @open
  Scenario: Edit an indexed coordinate of a shared dense page
    Given two pages sharing an immutable literal
    When the first byte of one page is modified
    Then the unchanged base and edited page restore exactly with one backing write

  @VR-20 @open
  Scenario: Serve concurrent updates to one guest page
    Given two writers updating disjoint byte ranges of the same page
    When the shared memory file accepts their writes
    Then both ranges remain exact and a failed replacement preserves the previous page

  @VR-20 @open
  Scenario: Write consecutive pages across a policy group boundary
    Given a full-page write spanning two Verdant policy groups
    When a later page fails to commit
    Then the committed prefix is readable and the failing page retains its previous bytes

  @VR-21 @open
  Scenario: Revalidate an independently fitted page
    Given a candidate fitted against a snapshot of the incumbent selector and regional search state
    When a concurrent operation changes those inputs or creates a matching shared literal
    Then the commit uses the current inputs and exact literal admission and restores the submitted bytes

  @VR-22 @build
  Scenario: Compare schedules for the same verified work
    Given a fit takes eight ticks and a second independent operation takes five
    When their execution fully overlaps and revalidation adds four ticks on the critical path
    Then the batch finishes earlier and adding five ticks would instead tie

  @VR-22 @build
  Scenario: A throughput improvement can still delay a page
    Given a page avoids three ticks of waiting
    When validation adds four ticks to that page
    Then the page has higher latency even if the batch finishes earlier

  @VR-22 @build
  Scenario: Choose throughput subject to a measured latency ceiling
    Given an incumbent satisfies a specified p99 ceiling on equal verified work
    When a faster trial meets the ceiling exactly
    Then the selected elapsed time is the faster trial's time

  @VR-23 @open
  Scenario: Reuse the page encoding after a neighboring fit changes the region
    Given a prepared fit whose selector still matches but whose region has changed
    When the page reaches exact admission and still needs a fit
    Then reuse the encoding and produce the same bytes and policy state as a fresh fit

  @VR-23 @open
  Scenario: A prepared refit crosses a regional credit update
    Given neighboring pages consume credits while a template refit is prepared
    When the live schedule still admits the same template
    Then the prepared refit advances the live credits and leaves the next mandatory probe due at its original boundary

  @VR-24 @build
  Scenario: Certify a tail rank at the deadline
    Given strict, equal and greater latency counts for a nonempty timed workload
    When the tied block contains the nearest rank at 99 percent
    Then at most one percent of pages are strictly slower and the strictly faster pages fall short of the rank

  @VR-24 @build
  Scenario: Shortening the longest worker exposes the next longest
    Given a worker takes twelve ticks and all remaining workers finish within nine ticks with the longest taking nine
    When that worker saves five ticks and the other durations and common overhead stay fixed
    Then elapsed time falls by three ticks

  @VR-25 @open
  Scenario: Read a run of dense pages sharing a literal with different edits
    Given adjacent pages in one group backed by the same immutable literal
    When the memory device reads the range
    Then it reads the literal once and applies each page's own edits to its output

  @VR-26 @open
  Scenario: Consecutive residual admissions examine the same literal
    Given a range write supplies different sparse changes to an existing file literal
    When several pages nominate the same live base
    Then the first admission reads its bytes and later admissions can reuse them while checking each exact reconstruction

  @VR-27 @build
  Scenario: Sweep newly eligible plans at each measured deadline
    Given two equal-work plans first qualify at the same deadline
    When the faster has been considered and a later deadline admits more plans
    Then the slower plan cannot improve the incumbent and earlier eligible plans need no second evaluation

  @VR-28 @open
  Scenario: Execute a signed projection on an owned DIMM page
    Given a guest holds a 64 by 64 byte matrix stable on its online Verdant DIMM
    When it supplies a signed weight per row and requests a projection after publication
    Then every returned column agrees with an independent guest computation and the reply identifies packed kernel execution when used

  @VR-05 @build
  Scenario: Reject a projection outside the selected DIMM
    Given a projection port scoped to one DIMM
    When the request has an unknown operation or an unaligned or out-of-range byte offset
    Then the device reports an invalid request

  @VR-29 @build
  Scenario: Compare eligible routes on verified equal work
    Given packed execution takes eight ticks and restore followed by CPU execution takes ten
    When resident execution takes two ticks but requires an existing guest copy
    Then packed execution is selected without that copy and resident CPU execution is selected with it

  @VR-30 @open
  Scenario: Stream more projection rows than fit in a single device transfer
    Given a guest keeps one online DIMM page stable and supplies successive signed weight rows
    When a batch crosses the bounded ioctl payload
    Then each output row agrees with its own guest CPU projection and the DIMM is published once for the request

  @VR-05 @build
  Scenario: Refuse an empty batch before accepting any additional weight rows
    Given a guest submits a batch header with zero rows
    When the broker validates the frame
    Then it reports an invalid request and can accept the next scalar frame

  @VR-31 @build
  Scenario: Count traffic for the two explicit routes on the existing projection port
    Given a complete batch has at least two rows and one to as many result tiles as rows
    When five rows are projected together
    Then the batch transfers fewer application bytes than restoring the page once, and a six-row batch transfers more

  @VR-32 @open
  Scenario: Read consecutive file slots with repeated bases and edits
    Given a range contains repeated and consecutive immutable file slots within a group
    When the DIMM restores the range
    Then it uses one exact file read for that run and reconstructs each logical page in order

  @VR-33 @build
  Scenario: Expand a compact slot run from the end of the output
    Given the source and destination start at zero and each destination advances one page
    When the next source repeats or advances one slot
    Then its index stays at or before the destination so later destinations can be written first

  @VR-34 @open
  Scenario: Boot to the configured ceiling with two bounded backing files
    Given the hardware and OS configuration define a target and no individual file can hold the reserve
    When the supplied directories together can reserve that target plus its replacement page
    Then the launcher starts at the configured total using one Verdant store covering both files

  @VR-35 @build
  Scenario: Split a required page count across backing volumes
    Given a first volume can hold three pages and a second can hold two
    When the device reserves five pages including its replacement slot
    Then the first reserve contributes three and the remaining two fit exactly

  @VR-36 @open
  Scenario: Qualify a workstation at its configured maximum
    Given backing for every page in the configured target and one replacement slot
    When the installed qualification writes distinct dense pages and reads the whole target
    Then every byte is verified and separate read and write rates cover the full target

  @VR-34 @open
  Scenario: Discover firmware capacity as the workstation user
    Given only root can read the SMBIOS table and the boot service has published its numeric capacity
    When the workstation user requests a capacity report
    Then the launcher can use the current boot's firmware maximum without exposing the table

  @VR-37 @build
  Scenario: Combine full-capacity service measurements
    Given each range supplies at least the work required by its measured time and the common throughput floor
    When both ranges contribute their work and time to the whole-device measurement
    Then the combined work satisfies the combined requirement

  @VR-37 @build @rejects
  Scenario: Refuse an unmet full-capacity throughput floor
    Given a verified one MiB extent and a one MiB per second read floor
    When reading takes one nanosecond longer than a second
    Then qualification returns "budget"

  @VR-38 @open
  Scenario: Inspect the product model from the published catalog
    Given the operator opens the facet catalog for this build
    When the catalog renders a feature, measure, threat or release record
    Then its record identifier resolves to the checked model and its evidence resolves to the repository

  @VR-39 @open
  Scenario: Read a facet from a standalone controller
    Given a verified Holo with no network capabilities
    When a client asks for the facet index and each named facet
    Then every returned group equals the checked product catalog

  @VR-05 @build @rejects
  Scenario: Refuse an unknown facet
    Given the released controller
    When the client requests F/missing
    Then the response is "invalid request"

  @VR-40 @build
  Scenario: Return the bytes of a matched archive facet
    Given the facet lookup found a payload
    When the model returns the lookup result
    Then the reply contains exactly the payload bytes

  @VR-15 @build
  Scenario: Replay a four GiB full-target qualification
    Given a 4096 MiB platform limit, an 8192 MiB guest limit and sufficient process address space
    When the client supplies 4294971392 backing bytes in the full-target request
    Then the model accepts a configured target of 4096 MiB

  @VR-15 @build @rejects
  Scenario: Detect a one-byte shortfall above the 32-bit boundary
    Given the same four GiB target
    When backing is one byte short of the full target and its spare page
    Then the response is "capacity mismatch"

  @VR-41 @open
  Scenario: Compare single-page swap device latency
    Given zram with zstd and the Verdant swap device in one disposable Linux guest
    When each backend receives identical synchronous 4 KiB writes and shuffled reads in three runs
    Then restored bytes match and each workload reports its median and its measured ratio to zram

  @VR-41 @open
  Scenario: Preserve data while its representation changes
    Given sparse, selector, periodic, perturbed, dense and uniform pages share a device
    When each address is overwritten with another family and read back in shuffled order
    Then each restored byte equals the last write and each family reports separate read and write medians

  @VR-42 @build
  Scenario: Allocate a measured zram deadline between transport and service
    Given upper bounds for both disjoint parts of a page operation
    When the two bounds together fit its deadline
    Then the complete operation fits the deadline

  @VR-43 @build
  Scenario: Replay zram parity for three trial medians
    Given three vRAM medians and three zram medians in half-nanosecond units
    When two vRAM medians are at most the middle zram observation
    Then the model accepts parity for that operation

  @VR-44 @build
  Scenario: Admit a compact exact factor by its storage bound
    Given the incumbent costs at least the alternative storage floor
    When the exact factor costs less than the floor
    Then its stored representation costs less than the incumbent

  @VR-45 @build
  Scenario: Reconstruct a page with a partial final repetition
    Given a verified displacement and a nonempty prefix of reconstructed bytes
    When the decoder copies the smaller of the available bytes and the remaining bytes
    Then every step stays inside the initialized prefix and consumes exactly that many remaining bytes

  @VR-46 @build
  Scenario: Partition an observed request and test a fixed deadline
    Given ordered timestamps for the system call and the nested store invocation
    When the two outside intervals and the store interval are added
    Then they equal the full duration and removing store time leaves the outside saving required above the deadline

  @VR-47 @open
  Scenario: Replay the timing of the same verified page request
    Given a diagnostic build of the current server and an uninstrumented build in the same guest
    When every sampled page is written and read back under the operator and mixed workloads
    Then the receipt accounts for every request and compares the observed outside-store medians with zram

  @VR-48 @build
  Scenario: Complete the request after all required effects
    Given a write with a rejected extent or a read that fails after its reply header
    When the adapter reports the result of its current effect
    Then the block policy drains the rejected payload before replying and stops a partial read without a second reply

  @VR-49 @open
  Scenario: Drive Linux wire requests through the linked model
    Given a single stream carrying reads, writes, trims, flushes and rejected commands
    When Linux requests successive effects on a real Verdant store
    Then every successful read returns the expected bytes and the next request retains its framing

  @VR-50 @build
  Scenario: Eliminate an impossible sparse base
    Given disjoint blocks and an upper bound on the total byte exceptions
    When more disjoint blocks contain an exception than that upper bound allows
    Then the sparse base is excluded and a uniform block longer than the bound excludes every other base

  @VR-51 @open
  Scenario: Preserve bytes across the sparse cost boundary
    Given pages with edits before, at and beyond the representation cost boundary
    When pages change from sparse to independent bytes and back
    Then each page restores exactly and the ordinary representation policy remains available beyond the boundary

  @VR-52 @open
  Scenario: Compare queue completion placement at equal work
    Given two fresh NBD devices serving the same Verdant binary in a disposable guest
    When one queue completes locally and the other retains the kernel default
    Then both settings are read back and Prism evaluates each paired workload median

  @VR-51 @open
  Scenario: Prove dominance of an exact non-dyadic repeat
    Given a nonuniform prefix repeated at an arbitrary displacement with a truncated final copy
    When its full overlap matches and the prefix costs less than every competing storage floor
    Then the store can admit the prefix before fitting those alternatives

  @VR-53 @build
  Scenario: Select a bounded block read window
    Given the remaining page count and the frame capacity
    When the model selects the next read window
    Then the result is the largest span that fits both counts and advances a nonempty read with available space

  @VR-53 @build
  Scenario: Recover the valid prefix after a failed range
    Given a nonempty request with frame capacity
    When a range read fails and the server resumes in single-page mode
    Then the model selects exactly one page

  @VR-54 @open
  Scenario: Read through window boundaries and a failing page
    Given a running block adapter and requests spanning multiple frames
    When the guest reads an intact range or a range containing a failing page
    Then intact bytes arrive in order and the first failing page follows the block error policy

  @VR-55 @build
  Scenario: Keep local pages inside a file read span
    Given a group containing modeled pages and file literals
    When the controller tests the next page in a span
    Then local pages require no source and file slots must repeat or advance by one within the extent

  @VR-56 @open
  Scenario: Restore mixed file and local values with a shared extent read
    Given consecutive file slots, selectors, holes and edited shared literals
    When a range crosses the intervening local values
    Then the store verifies every byte, counts restored selectors and stops coalescing at a gap or file boundary

  @VR-57 @build
  Scenario: Resolve a literal page operation
    Given a read or write on an aligned full page inside the declared extent
    When the literal controller resolves the operation
    Then it selects the requested transfer and refuses invalid shapes and unsupported operations

  @VR-58 @open
  Scenario: Measure resident literal page completion
    Given fresh equal-capacity literal, zram and shipping NBD devices in a disposable guest
    When the operator and mixed-overwrite workloads rotate across the devices
    Then all three return the submitted bytes and record independent read and write latencies

  @VR-59 @build
  Scenario: Account for exact fill and literal values
    Given the complete page equality observation and the resident directory charge
    When the Verdant controller chooses a representation
    Then uniform pages need only their directory entry and other pages keep all literal bytes

  @VR-60 @open
  Scenario: Recover mixed pages while accounting for physical allocations
    Given the fill device and fresh literal and zram controls
    When the guest overwrites the same slots with differing page shapes
    Then it verifies every read and records retained page and directory bytes alongside read and write latency

  @VR-61 @build
  Scenario: Admit a whole range before modifying its first page
    Given the adapter observes the operation, alignment, whole-page length and device boundary
    When the resident controller resolves the span
    Then only a complete in-bounds read or write receives a transfer action

  @VR-62 @open
  Scenario: Retire Linux swap slots after verified page recovery
    Given the packaged kernel store serving anonymous Linux memory
    When the guest pages out, restores data and drains the device
    Then every recovered byte matches and freed swap slots release their literal payloads

  @VR-62 @open
  Scenario: Compare bulk transfers on the kernel store and zram
    Given equal-capacity guest devices and identical data
    When direct requests grow from one to many pages
    Then each device returns the original bytes and records elapsed read and write times

  @VR-63 @build
  Scenario: Keep lock and payload distinguishable
    Given a fill byte or the address of a nonzero aligned literal frame
    When its slot carries the lock marker
    Then classification is preserved and removing the marker yields the original payload

  @VR-64 @open
  Scenario: Recover packed entries after concurrent replacement
    Given a packaged device with one directory word per logical page
    When fills and literal frames replace each other under direct IO and swap reclaim
    Then all bytes are recovered and the measured directory charge agrees with the slot count

  @VR-65 @build
  Scenario: Reduce an exactly repeated prefix
    Given two halves of the current kernel prefix
    When equality is checked and the allocation class can shrink
    Then only equal halves authorize a smaller prefix

  @VR-66 @open
  Scenario: Reuse and retire kernel repeat blocks
    Given a page written through the installed swap device
    When exact repetitions and near misses overwrite one another
    Then every byte reads back and retired payload disappears from the store charge
