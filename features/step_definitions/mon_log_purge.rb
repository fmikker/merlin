Given(/^I empty report_data$/) do
    h = @monlogpurgedbconfig
    Sequel.mysql2(:host => h.host, :port => h.port, :username => h.user, :password => h.pass, :database => h.name) do |db|
        db.run "truncate report_data"
        db_count = db[:report_data].count
        if db_count != 0
            fail("db is not empty")
        end
    end
end

Given(/^I insert some report_data$/) do |table|
    h = @monlogpurgedbconfig
    table.hashes.each do |obj|
        timestamp = obj["timestamp"]
        event_type = obj["event_type"]
        host_name = obj["host_name"]
        service_description = obj["service_description"]
        state = obj["state"]
        hard = obj["hard"]
        Sequel.mysql2(:host => h.host, :port => h.port, :username => h.user, :password => h.pass, :database => h.name) do |db|
            db.run "insert into report_data (timestamp, event_type, host_name, service_description, state, hard) values (#{timestamp}, #{event_type}, '#{host_name}', '#{service_description}', #{state}, #{hard})"
        end
    end
end

Given(/^table report_data has (\d+) entries?$/) do |num|
    h = @monlogpurgedbconfig
    Sequel.mysql2(:host => h.host, :port => h.port, :username => h.user, :password => h.pass, :database => h.name) do |db|
        db_times = db[:report_data].count
        if db_times != num.to_i
            fail("#{table} has #{db_times} entries, expected #{num}")
        end
    end
end

Given(/^table report_data contains? (\d+) matching rows?$/) do |times, values|
    values.map_headers! {|key| key.downcase.to_sym } # Symbolize keys
    h = @monlogpurgedbconfig
    Sequel.mysql2(:host => h.host, :port => h.port, :username => h.user, :password => h.pass, :database => h.name) do |db|
        db_times = db[:report_data].where(values.rows_hash).count
        if db_times != times.to_i
          fail("found #{db_times} line(s) in database but expected #{times} line(s)")
        end
    end
end