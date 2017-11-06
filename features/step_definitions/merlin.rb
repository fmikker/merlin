Given(/^([a-z0-9\-_]+) sends event ([A-Z_]+)$/) do |ref, event, values|
  defaults = {}
  if @merlin_packet_defaults.has_key? event then
    defaults = @merlin_packet_defaults[event].clone
  end

  # Allow multiple objects to be send
  values.transpose.hashes.each do |in_obj|
    # merge with default values and repack
    in_obj = defaults.merge(in_obj)
    step "#{ref} sends raw event #{event}", Cucumber::Ast::Table.new(in_obj.to_a)
  end
end

Then(/([a-z0-9\-_]+) (?:should appear|appears) disconnected$/) do |n|
  steps %Q{
    Given I ask query handler merlin nodeinfo
      | filter_var | filter_val | match_var | match_val |
      | name | #{n} | state | STATE_NONE |
  }
end

Then(/([a-z0-9\-_]+) (?:should appear|appears) connected$/) do |n|
  steps %Q{
    Given I ask query handler merlin nodeinfo
      | filter_var | filter_val | match_var | match_val |
      | name | #{n} | state | STATE_CONNECTED |
  }
end

Then(/([a-z0-9\-_]+) (?:should become|becomes) disconnected/) do |n|
  steps %Q{
    Then #{n} disconnects from merlin
    And #{n} appears disconnected
  }
end