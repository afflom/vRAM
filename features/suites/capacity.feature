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
