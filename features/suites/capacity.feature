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
